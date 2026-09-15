#include <algorithm>
#include <fstream>
#include <laso/schema/validator.hpp>
#include <nlohmann/json-schema.hpp>

namespace laso {
namespace {
std::string bounded_text(const std::string &value, std::size_t maximum) {
  return value.size() <= maximum ? value : value.substr(0, maximum);
}
bool remote(const std::string &ref) {
  return ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0 ||
         ref.rfind("ftp://", 0) == 0 || ref.find("://") != std::string::npos;
}
bool traversal(const std::filesystem::path &path) {
  for (const auto &part : path)
    if (part == "..")
      return true;
  return false;
}
bool inside(const std::filesystem::path &file, const std::filesystem::path &root) {
  std::error_code ec;
  auto rel = std::filesystem::relative(file, root, ec);
  return !ec && !rel.empty() && rel != "." && *rel.begin() != "..";
}
class Handler final : public nlohmann::json_schema::error_handler {
public:
  void error(const Json::json_pointer &instance, const Json &,
             const std::string &message) override {
    if (message_.empty()) {
      instance_ = instance.to_string();
      instance_ = bounded_text(instance_, 1024);
      message_ = bounded_text(message, 512);
    }
  }
  std::string instance_, message_;
};
void inspect_depth(const Json &value, unsigned depth, unsigned limit) {
  if (depth > limit)
    throw Error(ErrorCode::Validation, "Schema nesting depth limit exceeded");
  if (value.is_object())
    for (const auto &[key, child] : value.items()) {
      (void)key;
      inspect_depth(child, depth + 1, limit);
    }
  else if (value.is_array())
    for (const auto &child : value)
      inspect_depth(child, depth + 1, limit);
}
} // namespace

struct SchemaValidator::Loaded {
  std::filesystem::path path;
  Json document;
};

SchemaValidator::SchemaValidator(std::vector<std::filesystem::path> roots, SchemaLimits limits)
    : limits_(limits) {
  if (limits_.max_schema_bytes == 0 || limits_.max_payload_bytes == 0 || limits_.max_depth == 0 ||
      limits_.max_reference_documents == 0)
    throw Error(ErrorCode::Configuration, "Schema resource limits must be positive");
  if (roots.empty())
    roots.push_back(std::filesystem::current_path());
  std::set<std::string> seen_roots;
  for (const auto &root : roots) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical))
      throw Error(ErrorCode::Configuration, "Schema root is not an existing directory");
    if (seen_roots.insert(canonical.generic_string()).second)
      roots_.push_back(std::move(canonical));
  }
}

std::filesystem::path SchemaValidator::resolve_root_reference(const std::string &reference) const {
  if (reference.empty() || remote(reference) || reference.find('\0') != std::string::npos)
    throw Error(ErrorCode::Validation, "Schema reference is not a permitted local path");
  const auto hash = reference.find('#');
  const auto path_part = reference.substr(0, hash);
  if (path_part.empty())
    throw Error(ErrorCode::Validation, "A schema declaration must name a file");
  std::filesystem::path requested(path_part);
  if (requested.is_absolute() || traversal(requested))
    throw Error(ErrorCode::Validation, "Absolute schema paths are forbidden");
  for (const auto &root : roots_) {
    std::error_code ec;
    auto candidate = std::filesystem::weakly_canonical(root / requested, ec);
    if (!ec && std::filesystem::is_regular_file(candidate) && inside(candidate, root))
      return candidate;
  }
  throw Error(ErrorCode::Validation, "Schema file is missing or outside the allowed roots");
}

std::filesystem::path SchemaValidator::resolve_uri(const std::string &reference,
                                                   const std::filesystem::path &base) const {
  if (reference.empty())
    return base;
  if (remote(reference))
    throw Error(ErrorCode::Validation, "Remote schema references are forbidden");
  const auto hash = reference.find('#');
  const auto path_part = reference.substr(0, hash);
  if (path_part.empty())
    return base;
  std::filesystem::path requested(path_part);
  if (requested.is_absolute() || traversal(requested))
    throw Error(ErrorCode::Validation, "Schema references may not escape the allowed roots");
  std::error_code ec;
  auto candidate = std::filesystem::weakly_canonical(base.parent_path() / requested, ec);
  if (ec || !std::filesystem::is_regular_file(candidate))
    throw Error(ErrorCode::Validation, "Referenced schema file is missing");
  bool allowed = false;
  for (const auto &root : roots_)
    allowed = allowed || inside(candidate, root);
  if (!allowed)
    throw Error(ErrorCode::Validation, "Referenced schema escapes the allowed roots");
  return candidate;
}

std::shared_ptr<const SchemaValidator::Loaded>
SchemaValidator::load(const std::filesystem::path &path) const {
  const auto key = path.generic_string();
  {
    std::lock_guard lock(cache_mutex_);
    if (auto it = cache_.find(key); it != cache_.end())
      return it->second;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw Error(ErrorCode::Validation, "Cannot open schema file");
  std::string text(limits_.max_schema_bytes + 1, '\0');
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(file.gcount()));
  if (text.size() > limits_.max_schema_bytes)
    throw Error(ErrorCode::Validation, "Schema file exceeds configured size limit");
  auto document = Json::parse(text, nullptr, false);
  if (document.is_discarded() || !document.is_object())
    throw Error(ErrorCode::Validation, "Schema is not valid JSON");
  inspect_depth(document, 0, limits_.max_depth);
  auto loaded = std::make_shared<Loaded>(Loaded{path, std::move(document)});
  std::lock_guard lock(cache_mutex_);
  if (auto it = cache_.find(key); it != cache_.end())
    return it->second;
  if (cache_.size() < limits_.max_cached_schemas)
    cache_.emplace(key, loaded);
  return loaded;
}

void SchemaValidator::inspect_schema(const Json &schema, const std::filesystem::path &base,
                                     std::set<std::string> &seen, std::set<std::string> &active,
                                     unsigned depth, unsigned &documents) const {
  inspect_depth(schema, depth, limits_.max_depth);
  if (schema.is_object()) {
    if (schema.contains("$ref")) {
      if (!schema.at("$ref").is_string())
        throw Error(ErrorCode::Validation, "Schema $ref must be a string");
      const auto ref = schema.at("$ref").get<std::string>();
      if (remote(ref))
        throw Error(ErrorCode::Validation, "Remote schema references are forbidden");
      if (!ref.empty() && ref.front() != '#') {
        auto target = resolve_uri(ref, base);
        const auto key = target.generic_string();
        if (active.contains(key))
          throw Error(ErrorCode::Validation, "Cyclic external schema reference is forbidden");
        if (seen.insert(key).second) {
          if (++documents > limits_.max_reference_documents)
            throw Error(ErrorCode::Validation, "Schema reference limit exceeded");
          active.insert(key);
          try {
            inspect_schema(load(target)->document, target, seen, active, depth + 1, documents);
          } catch (...) {
            active.erase(key);
            throw;
          }
          active.erase(key);
        }
      }
    }
    for (const auto &[key, child] : schema.items()) {
      (void)key;
      inspect_schema(child, base, seen, active, depth + 1, documents);
    }
    if (schema.contains("type")) {
      static const std::set<std::string> types = {"null",   "boolean", "object", "array",
                                                  "number", "integer", "string"};
      const auto &type = schema.at("type");
      if (!(type.is_string() && types.contains(type.get<std::string>())) &&
          !(type.is_array() && std::all_of(type.begin(), type.end(), [](const Json &item) {
              return item.is_string() && types.contains(item.get<std::string>());
            })))
        throw Error(ErrorCode::Validation, "Schema type keyword is invalid");
    }
  } else if (schema.is_array())
    for (const auto &child : schema)
      inspect_schema(child, base, seen, active, depth + 1, documents);
}

void SchemaValidator::validate_declaration(const std::string &reference) const {
  auto root = resolve_root_reference(reference);
  std::set<std::string> seen{root.generic_string()};
  std::set<std::string> active{root.generic_string()};
  unsigned documents = 1;
  auto loaded = load(root);
  inspect_schema(loaded->document, root, seen, active, 0, documents);
  auto loader = [this, root](const nlohmann::json_uri &uri, Json &value) {
    if (!uri.scheme().empty() || !uri.authority().empty())
      throw Error(ErrorCode::Validation, "Remote schema references are forbidden");
    auto reference = uri.path();
    if (reference.starts_with('/'))
      reference.erase(0, 1);
    auto path = resolve_uri(reference, root);
    value = load(path)->document;
  };
  try {
    nlohmann::json_schema::json_validator validator(loader);
    validator.set_root_schema(loaded->document);
  } catch (const std::exception &) {
    throw Error(ErrorCode::Validation, "Schema definition is invalid");
  }
}

void SchemaValidator::validate(const std::string &reference, const Json &payload,
                               const std::string &node_id, const std::string &direction) const {
  if (reference.empty())
    return;
  try {
    if (payload.dump().size() > limits_.max_payload_bytes)
      throw Error(ErrorCode::Validation, "Payload exceeds schema validation size limit");
    inspect_depth(payload, 0, limits_.max_depth);
    auto root = resolve_root_reference(reference);
    std::set<std::string> seen{root.generic_string()};
    std::set<std::string> active{root.generic_string()};
    unsigned documents = 1;
    auto loaded = load(root);
    inspect_schema(loaded->document, root, seen, active, 0, documents);
    auto loader = [this, root](const nlohmann::json_uri &uri, Json &value) {
      if (!uri.scheme().empty() || !uri.authority().empty())
        throw Error(ErrorCode::Validation, "Remote schema references are forbidden");
      auto reference = uri.path();
      if (reference.starts_with('/'))
        reference.erase(0, 1);
      auto path = resolve_uri(reference, root);
      value = load(path)->document;
    };
    nlohmann::json_schema::json_validator validator(loader);
    validator.set_root_schema(loaded->document);
    Handler handler;
    validator.validate(payload, handler);
    if (!handler.message_.empty())
      throw Error(ErrorCode::Validation, "Schema validation failed",
                  {{"schema", reference},
                   {"node", node_id},
                   {"direction", direction},
                   {"instance_path", handler.instance_},
                   {"message", handler.message_}});
  } catch (const Error &error) {
    Json details{{"schema", bounded_text(reference, 512)},
                 {"node", bounded_text(node_id, 128)},
                 {"direction", bounded_text(direction, 32)}};
    if (error.details.is_object())
      for (const auto &[key, value] : error.details.items())
        if (key == "instance_path" || key == "schema_path" || key == "message")
          details[key] = value;
    throw Error(ErrorCode::Validation, error.what(), std::move(details));
  } catch (const std::exception &) {
    throw Error(ErrorCode::Validation, "Schema validation failed",
                {{"schema", bounded_text(reference, 512)},
                 {"node", bounded_text(node_id, 128)},
                 {"direction", bounded_text(direction, 32)}});
  }
}
} // namespace laso
