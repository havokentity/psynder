// SPDX-License-Identifier: MIT
// Psynder scene cooker: hand-authored .psyscene.json -> cooked SoA .psyscene.

#include "SceneCook.h"

#include "core/Types.h"
#include "math/Math.h"
#include "scene/SceneFile.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace psynder;

namespace {

struct Json {
    enum class Kind : u8 {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    f64 number = 0.0;
    std::string text;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    [[nodiscard]] const Json* field(std::string_view key) const noexcept {
        if (kind != Kind::Object)
            return nullptr;
        for (const auto& [k, v] : object) {
            if (k == key)
                return &v;
        }
        return nullptr;
    }
};

class JsonParser {
   public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(Json& out) {
        skip_ws();
        if (!parse_value(out))
            return false;
        skip_ws();
        if (pos_ != input_.size())
            return fail("trailing input");
        return true;
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

   private:
    std::string_view input_;
    usize pos_ = 0u;
    std::string error_;

    [[nodiscard]] char peek() const noexcept { return pos_ < input_.size() ? input_[pos_] : '\0'; }

    void skip_ws() noexcept {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool consume(char c) noexcept {
        if (peek() != c)
            return false;
        ++pos_;
        return true;
    }

    bool fail(std::string_view message) {
        if (error_.empty()) {
            error_ = "scene_cook json: ";
            error_ += message;
            error_ += " at byte ";
            error_ += std::to_string(pos_);
        }
        return false;
    }

    bool literal(std::string_view s) noexcept {
        if (input_.substr(pos_, s.size()) != s)
            return false;
        pos_ += s.size();
        return true;
    }

    bool parse_value(Json& out) {
        skip_ws();
        const char c = peek();
        if (c == '"') {
            out.kind = Json::Kind::String;
            return parse_string(out.text);
        }
        if (c == '{')
            return parse_object(out);
        if (c == '[')
            return parse_array(out);
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(out);
        if (literal("true")) {
            out.kind = Json::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.kind = Json::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (literal("null")) {
            out.kind = Json::Kind::Null;
            return true;
        }
        return fail("expected value");
    }

    bool parse_string(std::string& out) {
        if (!consume('"'))
            return fail("expected string");
        out.clear();
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size())
                return fail("unterminated string escape");
            const char esc = input_[pos_++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    if (pos_ + 4u > input_.size())
                        return fail("short unicode escape");
                    pos_ += 4u;
                    out.push_back('?');
                    break;
                default:
                    return fail("bad string escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Json& out) {
        const usize start = pos_;
        if (peek() == '-')
            ++pos_;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9')
                ++pos_;
        } else {
            return fail("bad number");
        }
        if (peek() == '.') {
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9'))
                return fail("bad number fraction");
            while (peek() >= '0' && peek() <= '9')
                ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-')
                ++pos_;
            if (!(peek() >= '0' && peek() <= '9'))
                return fail("bad number exponent");
            while (peek() >= '0' && peek() <= '9')
                ++pos_;
        }
        const std::string tmp{input_.substr(start, pos_ - start)};
        char* end = nullptr;
        const f64 value = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str())
            return fail("bad number");
        out.kind = Json::Kind::Number;
        out.number = value;
        return true;
    }

    bool parse_array(Json& out) {
        if (!consume('['))
            return fail("expected array");
        out.kind = Json::Kind::Array;
        out.array.clear();
        skip_ws();
        if (consume(']'))
            return true;
        while (true) {
            Json item;
            if (!parse_value(item))
                return false;
            out.array.push_back(std::move(item));
            skip_ws();
            if (consume(']'))
                return true;
            if (!consume(','))
                return fail("expected ',' or ']'");
        }
    }

    bool parse_object(Json& out) {
        if (!consume('{'))
            return fail("expected object");
        out.kind = Json::Kind::Object;
        out.object.clear();
        skip_ws();
        if (consume('}'))
            return true;
        while (true) {
            std::string key;
            skip_ws();
            if (!parse_string(key))
                return false;
            skip_ws();
            if (!consume(':'))
                return fail("expected ':'");
            Json value;
            if (!parse_value(value))
                return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (consume('}'))
                return true;
            if (!consume(','))
                return fail("expected ',' or '}'");
        }
    }
};

struct CookScene {
    scene::SceneFileEnvironment environment{};
    std::vector<math::Vec3> translations;
    std::vector<math::Quat> rotations;
    std::vector<math::Vec3> scales;
    std::vector<scene::SceneFileCamera> cameras;
    std::vector<scene::SceneFileMeshInstance> mesh_instances;
    std::vector<scene::SceneFileMaterial> materials;
    std::vector<char> strings{'\0'};
    std::unordered_map<std::string, u32> string_offsets;
};

[[nodiscard]] bool is_number(const Json* v) noexcept {
    return v && v->kind == Json::Kind::Number;
}

[[nodiscard]] bool is_array(const Json* v) noexcept {
    return v && v->kind == Json::Kind::Array;
}

[[nodiscard]] bool read_bool(const Json* v, bool fallback) noexcept {
    return v && v->kind == Json::Kind::Bool ? v->boolean : fallback;
}

[[nodiscard]] f32 read_f32(const Json* v, f32 fallback) noexcept {
    if (!is_number(v))
        return fallback;
    return static_cast<f32>(v->number);
}

[[nodiscard]] bool read_vec3(const Json* v, math::Vec3& out) noexcept {
    if (!is_array(v) || v->array.size() != 3u)
        return false;
    if (!is_number(&v->array[0]) || !is_number(&v->array[1]) || !is_number(&v->array[2]))
        return false;
    out.x = static_cast<f32>(v->array[0].number);
    out.y = static_cast<f32>(v->array[1].number);
    out.z = static_cast<f32>(v->array[2].number);
    return true;
}

[[nodiscard]] u8 hex_nibble(char c, bool& ok) noexcept {
    if (c >= '0' && c <= '9')
        return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<u8>(10 + c - 'a');
    if (c >= 'A' && c <= 'F')
        return static_cast<u8>(10 + c - 'A');
    ok = false;
    return 0u;
}

[[nodiscard]] u8 hex_byte(std::string_view s, bool& ok) noexcept {
    return static_cast<u8>((hex_nibble(s[0], ok) << 4u) | hex_nibble(s[1], ok));
}

[[nodiscard]] u32 read_color_rgba8(const Json* v, u32 fallback) noexcept {
    if (!v || v->kind != Json::Kind::String)
        return fallback;
    std::string_view s{v->text};
    if (s.size() != 7u || s[0] != '#')
        return fallback;
    bool ok = true;
    const u8 r = hex_byte(s.substr(1, 2), ok);
    const u8 g = hex_byte(s.substr(3, 2), ok);
    const u8 b = hex_byte(s.substr(5, 2), ok);
    if (!ok)
        return fallback;
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8u) |
           (static_cast<u32>(b) << 16u) | (0xFFu << 24u);
}

[[nodiscard]] scene::ObjectMobility read_mobility(const Json* v) noexcept {
    if (!v || v->kind != Json::Kind::String)
        return scene::ObjectMobility::Dynamic;
    if (v->text == "static")
        return scene::ObjectMobility::Static;
    return scene::ObjectMobility::Dynamic;
}

[[nodiscard]] u32 add_string(CookScene& scene, std::string_view value) {
    const std::string key{value};
    if (const auto it = scene.string_offsets.find(key); it != scene.string_offsets.end())
        return it->second;
    const u32 offset = static_cast<u32>(scene.strings.size());
    scene.strings.insert(scene.strings.end(), key.begin(), key.end());
    scene.strings.push_back('\0');
    scene.string_offsets.emplace(key, offset);
    return offset;
}

[[nodiscard]] render::MaterialFlags material_flag_from_name(std::string_view name) noexcept {
    if (name == "rasterVisible")
        return render::MaterialFlags::RasterVisible;
    if (name == "rtVisible")
        return render::MaterialFlags::RtVisible;
    if (name == "castsRtShadow")
        return render::MaterialFlags::CastsRtShadow;
    if (name == "receivesRtShadow")
        return render::MaterialFlags::ReceivesRtShadow;
    if (name == "castsRasterShadow")
        return render::MaterialFlags::CastsRasterShadow;
    if (name == "receivesRasterShadow")
        return render::MaterialFlags::ReceivesRasterShadow;
    if (name == "editable")
        return render::MaterialFlags::Editable;
    if (name == "bakeVisible")
        return render::MaterialFlags::BakeVisible;
    if (name == "castsBakedShadow")
        return render::MaterialFlags::CastsBakedShadow;
    if (name == "receivesBakedShadow")
        return render::MaterialFlags::ReceivesBakedShadow;
    if (name == "emissiveBakes")
        return render::MaterialFlags::EmissiveBakes;
    return render::MaterialFlags::None;
}

[[nodiscard]] render::MaterialFlags read_material_flags(const Json* v,
                                                        render::MaterialFlags fallback) noexcept {
    if (!v)
        return fallback;
    if (v->kind == Json::Kind::Number)
        return static_cast<render::MaterialFlags>(static_cast<u32>(v->number));
    if (v->kind == Json::Kind::String)
        return material_flag_from_name(v->text);
    if (!is_array(v))
        return fallback;
    render::MaterialFlags out = render::MaterialFlags::None;
    for (const Json& entry : v->array) {
        if (entry.kind == Json::Kind::String)
            out |= material_flag_from_name(entry.text);
    }
    return out;
}

[[nodiscard]] u32 add_transform(CookScene& scene, const Json& object) {
    math::Vec3 translation{0.0f, 0.0f, 0.0f};
    math::Vec3 scale{1.0f, 1.0f, 1.0f};
    math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};

    if (const Json* transform = object.field("transform")) {
        (void)read_vec3(transform->field("translation"), translation);
        (void)read_vec3(transform->field("position"), translation);
        (void)read_vec3(transform->field("scale"), scale);
    }
    (void)read_vec3(object.field("translation"), translation);
    (void)read_vec3(object.field("position"), translation);
    (void)read_vec3(object.field("scale"), scale);

    math::Vec3 axis{0.0f, 1.0f, 0.0f};
    f32 degrees = 0.0f;
    if (const Json* transform = object.field("transform")) {
        (void)read_vec3(transform->field("rotationAxis"), axis);
        degrees = read_f32(transform->field("rotationDegrees"), degrees);
    }
    (void)read_vec3(object.field("rotationAxis"), axis);
    degrees = read_f32(object.field("rotationDegrees"), degrees);
    if (degrees != 0.0f)
        rotation = math::quat_from_axis_angle(axis, degrees * math::kDegToRad);

    const u32 index = static_cast<u32>(scene.translations.size());
    scene.translations.push_back(translation);
    scene.rotations.push_back(rotation);
    scene.scales.push_back(scale);
    return index;
}

[[nodiscard]] bool cook_json(const Json& root, CookScene& out, std::string& error) {
    if (root.kind != Json::Kind::Object) {
        error = "scene root must be an object";
        return false;
    }
    if (const Json* env = root.field("environment")) {
        out.environment.clear_color_rgba8 =
            read_color_rgba8(env->field("clearColor"), out.environment.clear_color_rgba8);
        out.environment.clear_color = read_bool(env->field("clearColorEnabled"), true) ? 1u : 0u;
        out.environment.clear_depth = read_bool(env->field("clearDepth"), true) ? 1u : 0u;
    }

    if (const Json* cameras = root.field("cameras")) {
        if (!is_array(cameras)) {
            error = "cameras must be an array";
            return false;
        }
        out.cameras.reserve(cameras->array.size());
        for (const Json& c : cameras->array) {
            if (c.kind != Json::Kind::Object) {
                error = "camera entry must be an object";
                return false;
            }
            scene::SceneFileCamera camera{};
            camera.transform_index = add_transform(out, c);
            (void)read_vec3(c.field("lookAt"), camera.look_at);
            (void)read_vec3(c.field("up"), camera.up);
            camera.fov_y_rad = read_f32(c.field("fovYDegrees"), 60.0f) * math::kDegToRad;
            camera.near_z = read_f32(c.field("nearZ"), camera.near_z);
            camera.far_z = read_f32(c.field("farZ"), camera.far_z);
            camera.tile_w = static_cast<u32>(read_f32(c.field("tileW"), static_cast<f32>(camera.tile_w)));
            camera.tile_h = static_cast<u32>(read_f32(c.field("tileH"), static_cast<f32>(camera.tile_h)));
            camera.active = read_bool(c.field("active"), true) ? 1u : 0u;
            out.cameras.push_back(camera);
        }
    }

    if (const Json* materials = root.field("materials")) {
        if (!is_array(materials)) {
            error = "materials must be an array";
            return false;
        }
        out.materials.reserve(materials->array.size());
        for (const Json& m : materials->array) {
            if (m.kind != Json::Kind::Object) {
                error = "material entry must be an object";
                return false;
            }
            const Json* name = m.field("name");
            if (!name || name->kind != Json::Kind::String || name->text.empty()) {
                error = "material entry requires a non-empty name string";
                return false;
            }

            scene::SceneFileMaterial material{};
            material.name_offset = add_string(out, name->text);
            if (const Json* base_color_texture = m.field("baseColorTexture");
                base_color_texture && base_color_texture->kind == Json::Kind::String) {
                material.base_color_texture_name_offset = add_string(out, base_color_texture->text);
            }
            material.albedo_rgba8 = read_color_rgba8(m.field("albedo"), material.albedo_rgba8);
            material.flags = read_material_flags(m.field("flags"), material.flags);
            material.alpha_cutoff = read_f32(m.field("alphaCutoff"), material.alpha_cutoff);
            material.reflectivity = read_f32(m.field("reflectivity"), material.reflectivity);
            material.roughness = read_f32(m.field("roughness"), material.roughness);
            material.emissive = read_f32(m.field("emissive"), material.emissive);
            out.materials.push_back(material);
        }
    }

    const Json* meshes = root.field("meshInstances");
    if (!meshes)
        meshes = root.field("objects");
    if (meshes) {
        if (!is_array(meshes)) {
            error = "meshInstances must be an array";
            return false;
        }
        out.mesh_instances.reserve(meshes->array.size());
        for (const Json& m : meshes->array) {
            if (m.kind != Json::Kind::Object) {
                error = "mesh instance entry must be an object";
                return false;
            }
            const Json* mesh_name = m.field("mesh");
            if (!mesh_name || mesh_name->kind != Json::Kind::String || mesh_name->text.empty()) {
                error = "mesh instance requires a non-empty mesh string";
                return false;
            }
            scene::SceneFileMeshInstance instance{};
            instance.transform_index = add_transform(out, m);
            instance.mesh_name_offset = add_string(out, mesh_name->text);
            if (const Json* material = m.field("material");
                material && material->kind == Json::Kind::String) {
                instance.material_name_offset = add_string(out, material->text);
            }
            if (const Json* group = m.field("group");
                group && group->kind == Json::Kind::String) {
                instance.group_name_offset = add_string(out, group->text);
            }
            instance.mobility = read_mobility(m.field("mobility"));
            instance.flags = read_bool(m.field("visible"), true) ? scene::RenderableFlags::Visible
                                                                 : scene::RenderableFlags::None;
            out.mesh_instances.push_back(instance);
        }
    }
    return true;
}

void append_bytes(std::vector<u8>& out, const void* data, usize bytes) {
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + bytes);
}

void pad_to_alignment(std::vector<u8>& out) {
    const usize aligned =
        ((out.size() + scene::kPsySceneAlignment - 1u) / scene::kPsySceneAlignment) *
        scene::kPsySceneAlignment;
    out.resize(aligned, 0u);
}

template <class T>
void append_chunk(std::vector<u8>& bytes,
                  std::vector<scene::SceneFileChunk>& chunks,
                  scene::SceneFileChunkType type,
                  std::span<const T> data,
                  u32 stride) {
    pad_to_alignment(bytes);
    scene::SceneFileChunk chunk{};
    chunk.type = type;
    chunk.offset = static_cast<u32>(bytes.size());
    chunk.bytes = static_cast<u32>(data.size_bytes());
    chunk.stride = stride;
    if (!data.empty())
        append_bytes(bytes, data.data(), data.size_bytes());
    chunks.push_back(chunk);
}

[[nodiscard]] std::vector<u8> write_scene_blob(const CookScene& scene) {
    std::vector<u8> bytes;
    std::vector<scene::SceneFileChunk> chunks;
    bytes.resize(sizeof(scene::SceneFileHeader));
    bytes.resize(bytes.size() + 8u * sizeof(scene::SceneFileChunk));

    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Strings,
                 std::span<const char>{scene.strings.data(), scene.strings.size()},
                 1u);
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Environment,
                 std::span<const scene::SceneFileEnvironment>{&scene.environment, 1u},
                 sizeof(scene::SceneFileEnvironment));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::TransformTranslation,
                 std::span<const math::Vec3>{scene.translations.data(), scene.translations.size()},
                 sizeof(math::Vec3));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::TransformRotation,
                 std::span<const math::Quat>{scene.rotations.data(), scene.rotations.size()},
                 sizeof(math::Quat));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::TransformScale,
                 std::span<const math::Vec3>{scene.scales.data(), scene.scales.size()},
                 sizeof(math::Vec3));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Cameras,
                 std::span<const scene::SceneFileCamera>{scene.cameras.data(), scene.cameras.size()},
                 sizeof(scene::SceneFileCamera));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::MeshInstances,
                 std::span<const scene::SceneFileMeshInstance>{scene.mesh_instances.data(),
                                                               scene.mesh_instances.size()},
                 sizeof(scene::SceneFileMeshInstance));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Materials,
                 std::span<const scene::SceneFileMaterial>{scene.materials.data(),
                                                           scene.materials.size()},
                 sizeof(scene::SceneFileMaterial));

    scene::SceneFileHeader header{};
    header.file_bytes = static_cast<u32>(bytes.size());
    header.chunk_count = static_cast<u32>(chunks.size());
    header.transform_count = static_cast<u32>(scene.translations.size());
    header.camera_count = static_cast<u32>(scene.cameras.size());
    header.mesh_instance_count = static_cast<u32>(scene.mesh_instances.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), chunks.data(), chunks.size() * sizeof(chunks[0]));
    return bytes;
}

[[nodiscard]] bool read_text_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    return true;
}

[[nodiscard]] bool write_binary_file(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

}  // namespace

bool psynder::tools::cook_psyscene_json_file(const std::filesystem::path& input,
                                             const std::filesystem::path& output,
                                             SceneCookStats* stats,
                                             std::string* error) {
    std::string source;
    if (!read_text_file(input, source)) {
        if (error)
            *error = "failed to read " + input.string();
        return false;
    }

    Json root;
    JsonParser parser{source};
    if (!parser.parse(root)) {
        if (error)
            *error = parser.error();
        return false;
    }

    CookScene scene;
    std::string cook_error;
    if (!cook_json(root, scene, cook_error)) {
        if (error)
            *error = cook_error;
        return false;
    }

    const std::vector<u8> blob = write_scene_blob(scene);
    if (!write_binary_file(output, std::span<const u8>{blob.data(), blob.size()})) {
        if (error)
            *error = "failed to write " + output.string();
        return false;
    }

    if (stats) {
        stats->bytes = blob.size();
        stats->transforms = scene.translations.size();
        stats->cameras = scene.cameras.size();
        stats->mesh_instances = scene.mesh_instances.size();
    }
    return true;
}

int psynder::tools::scene_cook_cli_main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: scene_cook <input.psyscene.json> <output.psyscene>\n");
        return EXIT_FAILURE;
    }

    std::string error;
    SceneCookStats stats{};
    if (!cook_psyscene_json_file(argv[1], argv[2], &stats, &error)) {
        std::fprintf(stderr, "scene_cook: %s\n", error.c_str());
        return EXIT_FAILURE;
    }

    std::fprintf(stdout,
                 "scene_cook: %s -> %s (%zu bytes, %zu transforms, %zu cameras, %zu meshes)\n",
                 argv[1],
                 argv[2],
                 stats.bytes,
                 stats.transforms,
                 stats.cameras,
                 stats.mesh_instances);
    return EXIT_SUCCESS;
}
