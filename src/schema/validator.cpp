#include <algorithm>
#include <fstream>
#include <laso/schema/validator.hpp>
#include <nlohmann/json-schema.hpp>

namespace laso {
namespace {
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
      message_ = message;
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
  if (roots.empty())
    roots.push_back(std::filesystem::current_path());
  for (const auto &root : roots) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical))
      throw Error(ErrorCode::Configuration, "Schema root is not an existing directory");
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
  if (reference.empty() || remote(reference))
    throw Error(ErrorCode::Validation, "Remote schema references are forbidden");
  const auto hash = reference.find('#');
  const auto path_part = reference.substr(0, hash);
  if (path_part.empty())
    return base;
  std::filesystem::path requested(path_part);
  if (traversal(requested))
    throw Error(ErrorCode::Validation, "Schema references may not contain parent traversal");
  std::error_code ec;
  auto candidate = std::filesystem::weakly_canonical(
      requested.is_absolute() ? requested : base.parent_path() / requested, ec);
  if ((!std::filesystem::is_regular_file(candidate) || ec) && requested.is_absolute()) {
    const auto relative = requested.relative_path();
    for (const auto &root : roots_) {
      auto rooted = std::filesystem::weakly_canonical(root / relative, ec);
      if (!ec && std::filesystem::is_regular_file(rooted)) {
        candidate = std::move(rooted);
        break;
      }
    }
  }
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
  auto [it, inserted] = cache_.emplace(key, loaded);
  return inserted ? loaded : it->second;
}

void SchemaValidator::inspect_schema(const Json &schema, const std::filesystem::path &base,
                                     std::set<std::string> &seen, unsigned depth,
                                     unsigned &documents) const {
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
        if (seen.insert(target.generic_string()).second) {
          if (++documents > limits_.max_reference_documents)
            throw Error(ErrorCode::Validation, "Schema reference limit exceeded");
          inspect_schema(load(target)->document, target, seen, depth + 1, documents);
        }
      }
    }
    for (const auto &[key, child] : schema.items()) {
      (void)key;
      inspect_schema(child, base, seen, depth + 1, documents);
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
      inspect_schema(child, base, seen, depth + 1, documents);
}

void SchemaValidator::validate_declaration(const std::string &reference) const {
  auto root = resolve_root_reference(reference);
  std::set<std::string> seen{root.generic_string()};
  unsigned documents = 1;
  auto loaded = load(root);
  inspect_schema(loaded->document, root, seen, 0, documents);
  auto loader = [this](const nlohmann::json_uri &uri, Json &value) {
    auto path = resolve_uri(uri.path(), std::filesystem::path(uri.path()));
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
  if (payload.dump().size() > limits_.max_payload_bytes)
    throw Error(ErrorCode::Validation, "Payload exceeds schema validation size limit",
                {{"schema", reference}, {"node", node_id}, {"direction", direction}});
  auto root = resolve_root_reference(reference);
  std::set<std::string> seen{root.generic_string()};
  unsigned documents = 1;
  auto loaded = load(root);
  inspect_schema(loaded->document, root, seen, 0, documents);
  auto loader = [this](const nlohmann::json_uri &uri, Json &value) {
    auto path = resolve_uri(uri.path(), std::filesystem::path(uri.path()));
    value = load(path)->document;
  };
  try {
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
  } catch (const Error &) {
    throw;
  } catch (const std::exception &) {
    throw Error(ErrorCode::Validation, "Schema validation failed",
                {{"schema", reference}, {"node", node_id}, {"direction", direction}});
  }
}
} // namespace laso
