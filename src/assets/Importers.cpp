// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "assets/Importers.h"

#include <cgltf.h>
#include <stb_image.h>
#include <ufbx.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>

namespace mage {

namespace fs = std::filesystem;

// ============================================================================ helpers
std::string PathToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path Utf8ToPath(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool ReadFileBytes(const fs::path& file, std::vector<uint8_t>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto size = in.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    if (!out.empty()) in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}

AssetType DetectAssetType(const fs::path& file) {
    std::string ext = PathToUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* e : {".gltf", ".glb", ".fbx", ".obj"}) if (ext == e) return AssetType::Model;
    for (const char* e : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr"}) if (ext == e) return AssetType::Image;
    for (const char* e : {".ttf", ".otf", ".ttc"}) if (ext == e) return AssetType::Font;
    for (const char* e : {".wav", ".mp3", ".flac", ".ogg"}) if (ext == e) return AssetType::Sound;
    return AssetType::Unknown;
}

const char* AssetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Model: return "model";
        case AssetType::Image: return "texture";
        case AssetType::Font: return "font";
        case AssetType::Sound: return "sound";
        default: return "unknown";
    }
}

size_t ModelData::TriangleCount() const {
    size_t n = 0;
    for (const ModelPart& p : parts) n += meshes[p.mesh].indices.size() / 3;
    return n;
}

namespace {

bool HasNonFinite(const glm::vec3& v) { return glm::any(glm::isnan(v)) || glm::any(glm::isinf(v)); }

// A referenced file must stay inside the model's folder: no absolute paths, no "..".
bool SafeRelative(const fs::path& p) {
    if (p.empty() || p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p) if (part == "..") return false;
    return true;
}

void ComputeNormals(MeshData& m) {
    for (Vertex& v : m.vertices) v.normal = glm::vec3(0.0f);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        Vertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
        const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
        a.normal += n; b.normal += n; c.normal += n;
    }
}

// Both renderers divide by normal length, so never hand them a degenerate one.
void Sanitize(MeshData& m) {
    for (Vertex& v : m.vertices) {
        const float len = glm::length(v.normal);
        v.normal = (len > 1e-8f && !HasNonFinite(v.normal)) ? v.normal / len : glm::vec3(0, 1, 0);
        if (glm::any(glm::isnan(v.uv)) || glm::any(glm::isinf(v.uv))) v.uv = glm::vec2(0.0f);
    }
}

bool Finish(ModelData& d, std::string& error) {
    if (d.parts.empty()) { error = "file contains no triangles"; return false; }
    glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
    for (const ModelPart& p : d.parts)
        for (const Vertex& v : d.meshes[p.mesh].vertices) {
            const glm::vec3 w = glm::vec3(p.transform * glm::vec4(v.position, 1.0f));
            if (HasNonFinite(w)) { error = "mesh contains NaN or infinite positions"; return false; }
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
        }
    d.bounds = AABB{mn, mx};
    return true;
}

void Warn(ModelData& d, const std::string& w) {
    if (std::find(d.warnings.begin(), d.warnings.end(), w) == d.warnings.end()) d.warnings.push_back(w);
}

// ============================================================================ glTF
cgltf_result ReadCallback(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(Utf8ToPath(path), bytes)) return cgltf_result_file_not_found;
    if (*size != 0 && bytes.size() < *size) return cgltf_result_data_too_short;
    void* mem = std::malloc(std::max<size_t>(bytes.size(), 1));
    if (!mem) return cgltf_result_out_of_memory;
    std::memcpy(mem, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = mem;
    return cgltf_result_success;
}
void ReleaseCallback(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

fs::path DecodeUri(const char* uri) {
    std::string s(uri);
    s.resize(cgltf_decode_uri(s.data()));
    return Utf8ToPath(s);
}
bool IsDataUri(const char* uri) { return std::strncmp(uri, "data:", 5) == 0; }

const cgltf_accessor* FindAttribute(const cgltf_primitive& p, cgltf_attribute_type type, int index = 0) {
    for (size_t i = 0; i < p.attributes_count; ++i)
        if (p.attributes[i].type == type && p.attributes[i].index == index) return p.attributes[i].data;
    return nullptr;
}

bool LoadPrimitive(const cgltf_primitive& p, MeshData& m, ModelData& d, std::string& error) {
    if (p.has_draco_mesh_compression) { error = "Draco-compressed meshes are not supported"; return false; }
    const cgltf_accessor* pos = FindAttribute(p, cgltf_attribute_type_position);
    if (!pos || pos->count == 0) return true;
    const cgltf_accessor* nrm = FindAttribute(p, cgltf_attribute_type_normal);
    const cgltf_accessor* uv = FindAttribute(p, cgltf_attribute_type_texcoord);
    if (FindAttribute(p, cgltf_attribute_type_joints)) Warn(d, "skinning is ignored (static pose)");
    if (FindAttribute(p, cgltf_attribute_type_color)) Warn(d, "vertex colors are ignored");
    if (p.targets_count) Warn(d, "morph targets are ignored");

    m.vertices.resize(pos->count);
    for (size_t i = 0; i < pos->count; ++i) {
        Vertex& v = m.vertices[i];
        v.normal = glm::vec3(0.0f);
        v.uv = glm::vec2(0.0f);
        if (!cgltf_accessor_read_float(pos, i, &v.position.x, 3)) { error = "unreadable vertex positions"; return false; }
        if (nrm && i < nrm->count) cgltf_accessor_read_float(nrm, i, &v.normal.x, 3);
        if (uv && i < uv->count) cgltf_accessor_read_float(uv, i, &v.uv.x, 2);
    }

    std::vector<uint32_t> idx;
    if (p.indices) {
        idx.resize(p.indices->count);
        for (size_t i = 0; i < idx.size(); ++i) {
            idx[i] = static_cast<uint32_t>(cgltf_accessor_read_index(p.indices, i));
            if (idx[i] >= m.vertices.size()) { error = "index out of range"; return false; }
        }
    } else {
        idx.resize(pos->count);
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<uint32_t>(i);
    }
    if (p.type == cgltf_primitive_type_triangles) {
        idx.resize(idx.size() / 3 * 3);
        m.indices = std::move(idx);
    } else if (p.type == cgltf_primitive_type_triangle_strip) {
        for (size_t i = 0; i + 2 < idx.size(); ++i) {
            if (i % 2 == 0) m.indices.insert(m.indices.end(), {idx[i], idx[i + 1], idx[i + 2]});
            else m.indices.insert(m.indices.end(), {idx[i + 1], idx[i], idx[i + 2]});
        }
    } else if (p.type == cgltf_primitive_type_triangle_fan) {
        for (size_t i = 1; i + 1 < idx.size(); ++i) m.indices.insert(m.indices.end(), {idx[0], idx[i], idx[i + 1]});
    } else {
        Warn(d, "points and lines are ignored");
        return true;
    }
    if (!nrm) ComputeNormals(m);
    Sanitize(m);
    return true;
}

bool ImportGltf(const fs::path& file, ModelData& d, std::string& error) {
    cgltf_options options{};
    options.file.read = ReadCallback;
    options.file.release = ReleaseCallback;
    cgltf_data* raw = nullptr;
    const std::string utf8 = PathToUtf8(file);
    if (cgltf_result r = cgltf_parse_file(&options, utf8.c_str(), &raw); r != cgltf_result_success) {
        error = std::format("not a valid glTF file (cgltf error {})", static_cast<int>(r));
        return false;
    }
    struct Guard { cgltf_data* p; ~Guard() { cgltf_free(p); } } guard{raw};

    for (size_t i = 0; i < raw->extensions_required_count; ++i) {
        const std::string ext = raw->extensions_required[i];
        static const char* kOk[] = {"KHR_materials_emissive_strength", "KHR_materials_clearcoat", "KHR_materials_transmission",
                                    "KHR_materials_ior", "KHR_mesh_quantization"};
        if (std::none_of(std::begin(kOk), std::end(kOk), [&](const char* k) { return ext == k; })) {
            error = "requires unsupported glTF extension " + ext;
            return false;
        }
    }
    const fs::path dir = file.parent_path();
    for (size_t i = 0; i < raw->buffers_count; ++i) {
        const char* uri = raw->buffers[i].uri;
        if (!uri || IsDataUri(uri)) continue;
        const fs::path rel = DecodeUri(uri);
        if (!SafeRelative(rel)) { error = std::string("buffer path leaves the model folder: ") + uri; return false; }
        d.dependencies.push_back(rel);
    }
    if (cgltf_result r = cgltf_load_buffers(&options, raw, utf8.c_str()); r != cgltf_result_success) {
        error = std::format("could not load buffers (cgltf error {})", static_cast<int>(r));
        return false;
    }
    if (cgltf_result r = cgltf_validate(raw); r != cgltf_result_success) {
        error = std::format("failed glTF validation (cgltf error {})", static_cast<int>(r));
        return false;
    }

    for (size_t i = 0; i < raw->images_count; ++i) {
        const cgltf_image& img = raw->images[i];
        ModelImage out;
        out.name = img.name ? img.name : std::format("image{}", i);
        if (img.buffer_view) {
            if (const uint8_t* p = cgltf_buffer_view_data(img.buffer_view)) out.encoded.assign(p, p + img.buffer_view->size);
        } else if (img.uri && IsDataUri(img.uri)) {
            const char* comma = std::strstr(img.uri, ";base64,");
            if (comma) {
                const char* b64 = comma + 8;
                size_t len = std::strlen(b64);
                while (len && b64[len - 1] == '=') --len;
                const size_t size = len * 3 / 4;
                void* mem = nullptr;
                if (cgltf_load_buffer_base64(&options, size, b64, &mem) == cgltf_result_success) {
                    out.encoded.assign(static_cast<uint8_t*>(mem), static_cast<uint8_t*>(mem) + size);
                    std::free(mem);
                }
            }
        } else if (img.uri) {
            const fs::path rel = DecodeUri(img.uri);
            if (SafeRelative(rel)) {
                out.file = dir / rel;
                d.dependencies.push_back(rel);
            } else Warn(d, std::string("image outside the model folder was skipped: ") + img.uri);
        }
        d.images.push_back(std::move(out));
    }

    auto texture = [&](const cgltf_texture_view& v) -> int {
        if (!v.texture || !v.texture->image) return -1;
        if (v.texcoord != 0) { Warn(d, "textures on a second UV set are ignored"); return -1; }
        if (v.has_transform) Warn(d, "KHR_texture_transform is ignored");
        return static_cast<int>(v.texture->image - raw->images);
    };
    for (size_t i = 0; i < raw->materials_count; ++i) {
        const cgltf_material& m = raw->materials[i];
        ModelMaterial out;
        out.name = m.name ? m.name : std::format("material{}", i);
        out.params.metallic = 1.0f;
        out.params.roughness = 1.0f;
        out.params.albedo = glm::vec3(1.0f);
        if (m.has_pbr_metallic_roughness) {
            const auto& pbr = m.pbr_metallic_roughness;
            out.params.albedo = glm::make_vec3(pbr.base_color_factor);
            out.params.alpha = pbr.base_color_factor[3];
            out.params.metallic = pbr.metallic_factor;
            out.params.roughness = pbr.roughness_factor;
            out.baseColor = texture(pbr.base_color_texture);
            out.metallicRoughness = texture(pbr.metallic_roughness_texture);
        }
        out.normal = texture(m.normal_texture);
        out.params.normalScale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
        out.emissive = texture(m.emissive_texture);
        out.params.emissive = glm::make_vec3(m.emissive_factor) * (m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f);
        if (m.has_clearcoat) {
            out.params.clearcoat = m.clearcoat.clearcoat_factor;
            out.params.clearcoatRoughness = std::max(m.clearcoat.clearcoat_roughness_factor, 0.02f);
        }
        if (m.has_transmission) out.params.transmission = m.transmission.transmission_factor;
        if (m.has_ior) out.params.ior = m.ior.ior;
        out.params.doubleSided = m.double_sided;
        if (m.alpha_mode == cgltf_alpha_mode_mask) out.params.alphaCutoff = std::max(m.alpha_cutoff, 1e-3f);
        else if (m.alpha_mode == cgltf_alpha_mode_blend) {
            out.params.alphaCutoff = 0.5f;
            Warn(d, "blended transparency is approximated by cut-out alpha");
        }
        d.materials.push_back(std::move(out));
    }
    int defaultMaterial = -1;
    auto materialIndex = [&](const cgltf_material* m) -> uint32_t {
        if (m) return static_cast<uint32_t>(m - raw->materials);
        if (defaultMaterial < 0) {
            defaultMaterial = static_cast<int>(d.materials.size());
            d.materials.emplace_back();
            d.materials.back().name = "default";
        }
        return static_cast<uint32_t>(defaultMaterial);
    };

    std::map<const cgltf_primitive*, int> loaded;
    auto addNode = [&](const cgltf_node& node) -> bool {
        if (!node.mesh) return true;
        float world[16];
        cgltf_node_transform_world(&node, world);
        for (size_t i = 0; i < node.mesh->primitives_count; ++i) {
            const cgltf_primitive& prim = node.mesh->primitives[i];
            auto it = loaded.find(&prim);
            if (it == loaded.end()) {
                MeshData mesh;
                if (!LoadPrimitive(prim, mesh, d, error)) return false;
                int index = -1;
                if (!mesh.indices.empty()) {
                    index = static_cast<int>(d.meshes.size());
                    d.meshes.push_back(std::move(mesh));
                }
                it = loaded.emplace(&prim, index).first;
            }
            if (it->second >= 0) d.parts.push_back({static_cast<uint32_t>(it->second), materialIndex(prim.material), glm::make_mat4(world)});
        }
        return true;
    };
    // Walk the default scene (or everything if the file has none).
    const cgltf_scene* scene = raw->scene ? raw->scene : (raw->scenes_count ? raw->scenes : nullptr);
    std::vector<const cgltf_node*> stack;
    if (scene) for (size_t i = 0; i < scene->nodes_count; ++i) stack.push_back(scene->nodes[i]);
    else for (size_t i = 0; i < raw->nodes_count; ++i) if (!raw->nodes[i].parent) stack.push_back(&raw->nodes[i]);
    while (!stack.empty()) {
        const cgltf_node* n = stack.back();
        stack.pop_back();
        if (!addNode(*n)) return false;
        for (size_t i = 0; i < n->children_count; ++i) stack.push_back(n->children[i]);
    }
    if (raw->animations_count) Warn(d, "animations are ignored");
    return Finish(d, error);
}

// ============================================================================ FBX / OBJ
glm::mat4 ToGlm(const ufbx_matrix& m) {
    glm::mat4 r(1.0f);
    for (int c = 0; c < 4; ++c)
        r[c] = glm::vec4(static_cast<float>(m.cols[c].x), static_cast<float>(m.cols[c].y), static_cast<float>(m.cols[c].z), c == 3 ? 1.0f : 0.0f);
    return r;
}

struct MtlContext {
    fs::path dir;
    std::vector<fs::path> opened;
};

// ufbx may open material libraries only, and only from inside the model's own folder.
bool OpenMtlCallback(void* user, ufbx_stream* stream, const char* path, size_t length, const ufbx_open_file_info* info) {
    auto& ctx = *static_cast<MtlContext*>(user);
    if (info->type != UFBX_OPEN_FILE_OBJ_MTL) return false;
    const fs::path rel = Utf8ToPath(std::string(path, length)).lexically_normal().lexically_relative(ctx.dir.lexically_normal());
    if (rel.empty() || !SafeRelative(rel)) return false;
    if (!ufbx_open_file(stream, path, length, nullptr, nullptr)) return false;
    if (std::find(ctx.opened.begin(), ctx.opened.end(), rel) == ctx.opened.end()) ctx.opened.push_back(rel);
    return true;
}

bool ImportUfbx(const fs::path& file, ModelData& d, std::string& error) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;
    opts.obj_search_mtl_by_filename = true;
    opts.ignore_animation = true;
    opts.load_external_files = true;
    opts.ignore_missing_external_files = true;
    opts.open_main_file_with_default = true;
    MtlContext mtl{file.parent_path(), {}};
    opts.open_file_cb.fn = OpenMtlCallback;
    opts.open_file_cb.user = &mtl;
    ufbx_error err;
    const std::string utf8 = PathToUtf8(file);
    ufbx_scene* scene = ufbx_load_file(utf8.c_str(), &opts, &err);
    if (!scene) {
        error = std::string("could not load: ") + std::string(err.description.data, err.description.length);
        return false;
    }
    struct Guard { ufbx_scene* s; ~Guard() { ufbx_free_scene(s); } } guard{scene};
    const fs::path dir = file.parent_path();
    std::error_code ec;

    std::map<const ufbx_texture*, int> textures;
    auto texture = [&](const ufbx_texture* t) -> int {
        if (!t) return -1;
        if (t->file_textures.count > 0) t = t->file_textures.data[0];
        if (auto it = textures.find(t); it != textures.end()) return it->second;
        ModelImage img;
        img.name = std::string(t->name.data, t->name.length);
        int index = -1;
        if (t->content.size > 0) {
            const auto* p = static_cast<const uint8_t*>(t->content.data);
            img.encoded.assign(p, p + t->content.size);
        } else {
            // Exporters often store absolute paths: look only next to the model.
            std::string rel(t->relative_filename.length ? t->relative_filename.data : t->filename.data,
                            t->relative_filename.length ? t->relative_filename.length : t->filename.length);
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const fs::path p = Utf8ToPath(rel);
            for (const fs::path& c : {p, p.filename(), fs::path("textures") / p.filename()})
                if (SafeRelative(c) && fs::is_regular_file(dir / c, ec)) {
                    img.file = dir / c;
                    d.dependencies.push_back(c);
                    break;
                }
            if (img.file.empty()) Warn(d, "texture not found next to the model: " + PathToUtf8(p.filename()));
        }
        if (!img.encoded.empty() || !img.file.empty()) {
            index = static_cast<int>(d.images.size());
            d.images.push_back(std::move(img));
        }
        textures[t] = index;
        return index;
    };

    std::map<const ufbx_material*, uint32_t> materials;
    auto materialIndex = [&](const ufbx_material* m) -> uint32_t {
        if (auto it = materials.find(m); it != materials.end()) return it->second;
        ModelMaterial out;
        out.name = m ? std::string(m->name.data, m->name.length) : "default";
        if (m) {
            const auto& pbr = m->pbr;
            const float base = pbr.base_factor.has_value ? static_cast<float>(pbr.base_factor.value_real) : 1.0f;
            if (pbr.base_color.has_value)
                out.params.albedo = glm::vec3(pbr.base_color.value_vec3.x, pbr.base_color.value_vec3.y, pbr.base_color.value_vec3.z) * base;
            out.baseColor = texture(pbr.base_color.texture);
            if (out.baseColor >= 0 && glm::all(glm::lessThan(out.params.albedo, glm::vec3(1e-4f)))) out.params.albedo = glm::vec3(1.0f);
            if (pbr.roughness.has_value) out.params.roughness = glm::clamp(static_cast<float>(pbr.roughness.value_real), 0.02f, 1.0f);
            if (pbr.metalness.has_value) out.params.metallic = glm::clamp(static_cast<float>(pbr.metalness.value_real), 0.0f, 1.0f);
            if (pbr.roughness.texture || pbr.metalness.texture) Warn(d, "separate roughness/metalness maps are ignored (glTF packs them)");
            out.normal = texture(pbr.normal_map.texture);
            if (pbr.emission_color.has_value) {
                const float f = pbr.emission_factor.has_value ? static_cast<float>(pbr.emission_factor.value_real) : 1.0f;
                out.params.emissive = glm::vec3(pbr.emission_color.value_vec3.x, pbr.emission_color.value_vec3.y, pbr.emission_color.value_vec3.z) * f;
            }
            out.emissive = texture(pbr.emission_color.texture);
            out.params.doubleSided = m->features.double_sided.enabled;
        }
        const auto index = static_cast<uint32_t>(d.materials.size());
        d.materials.push_back(std::move(out));
        materials[m] = index;
        return index;
    };

    std::map<std::pair<const ufbx_mesh*, size_t>, int> loaded;
    for (size_t n = 0; n < scene->nodes.count; ++n) {
        const ufbx_node* node = scene->nodes.data[n];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || !node->visible) continue;
        if (mesh->skin_deformers.count) Warn(d, "skinning is ignored (static pose)");
        if (mesh->blend_deformers.count) Warn(d, "blend shapes are ignored");
        std::vector<uint32_t> tri(std::max<size_t>(mesh->max_face_triangles * 3, 3));
        for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh->material_parts.data[pi];
            if (part.num_triangles == 0) continue;
            auto key = std::make_pair(mesh, pi);
            auto it = loaded.find(key);
            if (it == loaded.end()) {
                MeshData out;
                out.vertices.reserve(part.num_triangles * 3);
                for (size_t f = 0; f < part.face_indices.count; ++f) {
                    const ufbx_face face = mesh->faces.data[part.face_indices.data[f]];
                    const uint32_t count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                    for (uint32_t i = 0; i < count * 3; ++i) {
                        const uint32_t ix = tri[i];
                        Vertex v{};
                        const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                        v.position = glm::vec3(p.x, p.y, p.z);
                        if (mesh->vertex_normal.exists) {
                            const ufbx_vec3 q = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);
                            v.normal = glm::vec3(q.x, q.y, q.z);
                        }
                        if (mesh->vertex_uv.exists) {
                            const ufbx_vec2 q = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                            v.uv = glm::vec2(q.x, 1.0 - q.y); // FBX/OBJ put V=0 at the bottom
                        }
                        out.vertices.push_back(v);
                    }
                }
                std::vector<uint32_t> indices(out.vertices.size());
                ufbx_vertex_stream stream{out.vertices.data(), out.vertices.size(), sizeof(Vertex)};
                ufbx_error ie;
                const size_t unique = ufbx_generate_indices(&stream, 1, indices.data(), indices.size(), nullptr, &ie);
                int index = -1;
                if (unique > 0) {
                    out.vertices.resize(unique);
                    out.indices = std::move(indices);
                    Sanitize(out);
                    index = static_cast<int>(d.meshes.size());
                    d.meshes.push_back(std::move(out));
                }
                it = loaded.emplace(key, index).first;
            }
            if (it->second < 0) continue;
            const ufbx_material* mat = pi < node->materials.count ? node->materials.data[pi]
                                     : (pi < mesh->materials.count ? mesh->materials.data[pi] : nullptr);
            d.parts.push_back({static_cast<uint32_t>(it->second), materialIndex(mat), ToGlm(node->geometry_to_world)});
        }
    }
    // OBJ material libraries have to travel with the model.
    d.dependencies.insert(d.dependencies.end(), mtl.opened.begin(), mtl.opened.end());
    return Finish(d, error);
}

} // namespace

// ============================================================================ public API
bool ImportModel(const fs::path& file, ModelData& out, std::string& error) {
    out = ModelData{};
    error.clear();
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) { error = "file not found"; return false; }
    if (DetectAssetType(file) != AssetType::Model) { error = "unsupported model format"; return false; }
    std::string ext = PathToUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool ok = (ext == ".gltf" || ext == ".glb") ? ImportGltf(file, out, error) : ImportUfbx(file, out, error);
    if (!ok) out = ModelData{};
    return ok;
}

bool DecodeImage(const uint8_t* data, size_t size, ImageData& out, std::string& error) {
    out = ImageData{};
    if (size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) { error = "empty or oversized image"; return false; }
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &n)) { error = "unsupported or corrupt image"; return false; }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { error = std::format("image size {}x{} is out of range", w, h); return false; }
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
    if (!px) { error = "corrupt image data"; return false; }
    out.width = w;
    out.height = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool ImportImage(const fs::path& file, ImageData& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(file, bytes)) { error = "file not found"; return false; }
    return DecodeImage(bytes.data(), bytes.size(), out, error);
}

bool ValidateFont(const fs::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    char tag[4] = {};
    if (!in.read(tag, 4)) { error = "file not found or too short"; return false; }
    const char ttf[4] = {0, 1, 0, 0};
    if (!std::memcmp(tag, ttf, 4) || !std::memcmp(tag, "OTTO", 4) || !std::memcmp(tag, "true", 4) || !std::memcmp(tag, "ttcf", 4)) return true;
    error = "not a TrueType/OpenType font";
    return false;
}

} // namespace mage
