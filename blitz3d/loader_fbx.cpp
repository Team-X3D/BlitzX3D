#include "std.h"
#include "loader_fbx.h"
#include "meshmodel.h"
#include "pivot.h"
#include "camera.h"
#include "brush.h"
#include "texture.h"
#include "cachedtexture.h"
#include "animator.h"
#include "animation.h"

#include "../gxruntime/gxscene.h"
#include "../vendor/ufbx/ufbx.h"

#include <algorithm>
#include <cmath>

extern gxGraphics* gx_graphics;

namespace {

	static Transform conv_tform;
	static bool conv_active;
	static bool flip_tris;

	static std::string str(const ufbx_string& s) {
		return s.length ? std::string(s.data, s.length) : std::string();
	}

	static Vector vec(const ufbx_vec3& v) {
		return Vector(v.x, v.y, v.z);
	}

	static Vector convVec(const Vector& v) {
		return conv_active ? conv_tform * v : v;
	}

	static Vector convNrm(const Vector& n) {
		Vector t = n;
		if (conv_active) t = conv_tform.m.cofactor() * t;
		float l = t.length();
		return l > EPSILON ? t / l : Vector(0, 0, 1);
	}

	static Transform convTform(const Transform& t) {
		return conv_active ? conv_tform * t * -conv_tform : t;
	}

	static Transform matrixTform(const ufbx_matrix& m) {
		return Transform(Matrix(Vector(m.m00, m.m10, m.m20), Vector(m.m01, m.m11, m.m21), Vector(m.m02, m.m12, m.m22)), Vector(m.m03, m.m13, m.m23));
	}

	static Transform decomposed(const ufbx_transform& t) {
		return matrixTform(ufbx_transform_to_matrix(&t));
	}

	static Quat engineQuat(const ufbx_quat& q) {
		ufbx_transform t;
		t.translation.x = t.translation.y = t.translation.z = 0;
		t.rotation = q;
		t.scale.x = t.scale.y = t.scale.z = 1;
		return matrixQuat(matrixTform(ufbx_transform_to_matrix(&t)).m);
	}

	static Transform camera_correction() {
		return Transform(Matrix(Vector(0, 0, -1), Vector(0, 1, 0), Vector(1, 0, 0)));
	}

	static bool fileExists(const std::string& f) {
		DWORD attrs = GetFileAttributesA(f.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	static std::string texturePath(const ufbx_texture* tex, bool* exists) {
		*exists = false;
		const ufbx_string* candidates[] = { &tex->filename, &tex->relative_filename, &tex->absolute_filename };
		std::string fallback;
		for (int i = 0; i < 3; ++i) {
			std::string file = str(*candidates[i]);
			if (file.empty()) continue;
			if (fallback.empty()) fallback = file;
			if (fileExists(file)) {
				*exists = true;
				return file;
			}
		}
		return fallback;
	}

	static void addTri(int a, int b, int c, const Brush& brush) {
		if (flip_tris) std::swap(b, c);
		MeshLoader::addTriangle(a, b, c, brush);
	}

	struct Builder {
		ufbx_scene* scene;
		MeshModel* root;
		std::string path;
		bool collapse;
		bool animonly;
		std::map<const ufbx_node*, Entity*> objects;
		std::vector<Object*> anim_objects;
		std::map<const Object*, int> anim_index;
		std::map<const ufbx_material*, Brush> brushes;

		Builder() :scene(0), root(0), collapse(false), animonly(false) {
		}

		Brush makeBrush(const ufbx_material* mat) {
			if (!mat) return Brush();

			std::map<const ufbx_material*, Brush>::const_iterator it = brushes.find(mat);
			if (it != brushes.end()) return it->second;

			Brush b;
			ufbx_vec3 color = { 1, 1, 1 };
			if (mat->fbx.diffuse_color.has_value) color = mat->fbx.diffuse_color.value_vec3;
			else if (mat->pbr.base_color.has_value) color = mat->pbr.base_color.value_vec3;

			float alpha = 1;
			if (mat->fbx.transparency_factor.has_value) alpha = 1 - mat->fbx.transparency_factor.value_real;
			else if (mat->pbr.opacity.has_value) alpha = mat->pbr.opacity.value_real;
			alpha = std::clamp(alpha, 0.0f, 1.0f);

			b.setColor(Vector(color.x, color.y, color.z));
			b.setAlpha(alpha);
			if (alpha < 1) b.setBlend(gxScene::BLEND_ALPHA);
			if (mat->fbx.specular_exponent.has_value) b.setShininess(mat->fbx.specular_exponent.value_real);

			const ufbx_material_map* map = &mat->fbx.diffuse_color;
			if (!map->texture) map = &mat->pbr.base_color;
			if (map->texture && map->texture->type == UFBX_TEXTURE_FILE) {
				bool exists = false;
				std::string file = texturePath(map->texture, &exists);
				if (!file.empty()) {
					std::string old_path = path;
					if (exists) CachedTexture::setPath("");
					b.setTexture(0, Texture(exists ? file : filenamefile(file), 0), 0);
					if (exists) CachedTexture::setPath(old_path);
					b.setColor(Vector(1, 1, 1));
				}
			}

			brushes[mat] = b;
			return b;
		}

		int boneIndex(const ufbx_node* node)const {
			if (!node) return -1;
			std::map<const ufbx_node*, Entity*>::const_iterator it = objects.find(node);
			if (it == objects.end()) return -1;
			Object* obj = it->second->getObject();
			std::map<const Object*, int>::const_iterator jt = anim_index.find(obj);
			return jt == anim_index.end() ? -1 : jt->second;
		}

		void makeObjects() {
			for (size_t i = 0; i < scene->nodes.count; ++i) {
				ufbx_node* node = scene->nodes.data[i];
				if (node == scene->root_node) continue;

				Entity* obj;
				if (node->mesh && !animonly) obj = new MeshModel();
				else if (node->camera && !animonly) {
					Camera* cam = new Camera();
					cam->setViewport(0, 0, gx_graphics->getWidth(), gx_graphics->getHeight());
					cam->setClsColor(Vector(0, 0, 0));
					cam->setClsMode(true, true);
					float unit = scene->settings.unit_meters > 0 ? scene->settings.unit_meters : 1.0f;
					cam->setRange(node->camera->near_plane * unit, node->camera->far_plane * unit);
					float tan_half = node->camera->field_of_view_tan.x;
					if (tan_half > 0) cam->setZoom(1.0f / tan_half);
					obj = cam;
				}
				else obj = new Pivot();

				obj->setName(str(node->name));
				Transform local = decomposed(node->local_transform);
				if (node->camera) local = local * camera_correction();
				obj->setLocalTform(convTform(local));
				if (node->camera) obj->setLocalScale(Vector(1, 1, 1));
				obj->setVisible(node->visible);
				objects[node] = obj;
			}

			for (size_t i = 0; i < scene->nodes.count; ++i) {
				ufbx_node* node = scene->nodes.data[i];
				if (node == scene->root_node) continue;

				Entity* parent = root;
				if (node->parent && node->parent != scene->root_node) {
					std::map<const ufbx_node*, Entity*>::const_iterator it = objects.find(node->parent);
					if (it != objects.end()) parent = it->second;
				}
				objects[node]->setParent(parent);
			}
		}

		void collect(Entity* e, std::vector<Object*>& out) {
			if (Object* o = e->getObject()) out.push_back(o);
			for (Entity* c = e->children(); c; c = c->successor()) collect(c, out);
		}

		void bindPose() {
			if (collapse || animonly) return;

			std::map<const ufbx_node*, ufbx_matrix> bind;
			for (size_t i = 0; i < scene->meshes.count; ++i) {
				ufbx_mesh* mesh = scene->meshes.data[i];
				if (mesh->skin_deformers.count == 0) continue;
				ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
				for (size_t k = 0; k < skin->clusters.count; ++k) {
					ufbx_skin_cluster* cluster = skin->clusters.data[k];
					if (cluster->bone_node) bind[cluster->bone_node] = cluster->bind_to_world;
				}
			}

			std::vector<std::pair<int, const ufbx_node*> > order;
			order.reserve(bind.size());
			for (std::map<const ufbx_node*, ufbx_matrix>::const_iterator it = bind.begin(); it != bind.end(); ++it) {
				int depth = 0;
				for (const ufbx_node* n = it->first; n && n->parent; n = n->parent) ++depth;
				order.push_back(std::make_pair(depth, it->first));
			}
			std::stable_sort(order.begin(), order.end(), [](const std::pair<int, const ufbx_node*>& a, const std::pair<int, const ufbx_node*>& b) {
				return a.first < b.first;
			});

			for (size_t i = 0; i < order.size(); ++i) {
				const ufbx_node* node = order[i].second;
				std::map<const ufbx_node*, Entity*>::const_iterator it = objects.find(node);
				if (it == objects.end()) continue;

				Transform desired = convTform(matrixTform(bind[node]));
				Entity* bone = it->second;
				Entity* parent = bone->getParent();
				Transform parent_tform = parent ? parent->getWorldTform() : Transform();
				bone->setLocalTform(-parent_tform * desired);
			}
		}

		int bakeAnimation() {
			ufbx_baked_anim* bake = 0;
			ufbx_error error;
			ufbx_anim* anim = scene->anim;
			double duration = anim ? anim->time_end - anim->time_begin : 0;
			for (size_t i = 0; i < scene->anim_stacks.count; ++i) {
				ufbx_anim* stack = scene->anim_stacks.data[i]->anim;
				if (!stack) continue;
				double d = stack->time_end - stack->time_begin;
				if (d > duration) {
					duration = d;
					anim = stack;
				}
			}
			if (anim) {
				double fps = scene->settings.frames_per_second;
				if (fps < 1) fps = 30;

				ufbx_bake_opts opts = { 0 };
				opts.resample_rate = fps;
				opts.trim_start_time = false;
				bake = ufbx_bake_anim(scene, anim, &opts, &error);
			}

			int frames = 0;
			if (!bake) return 0;

			double fps = scene->settings.frames_per_second;
			if (fps < 1) fps = 30;

			for (size_t i = 0; i < bake->nodes.count; ++i) {
				ufbx_baked_node& bn = bake->nodes.data[i];
				if (bn.typed_id >= scene->nodes.count) continue;
				ufbx_node* node = scene->nodes.data[bn.typed_id];
				std::map<const ufbx_node*, Entity*>::const_iterator it = objects.find(node);
				if (it == objects.end()) continue;

				Object* obj = it->second->getObject();
				Animation anim = obj->getAnimation();

				for (size_t k = 0; k < bn.translation_keys.count; ++k) {
					const ufbx_baked_vec3& key = bn.translation_keys.data[k];
					int frame = (int)std::lround(key.time * fps);
					anim.setPositionKey(frame, convVec(vec(key.value)));
					if (frame > frames) frames = frame;
				}
				for (size_t k = 0; k < bn.rotation_keys.count; ++k) {
					const ufbx_baked_quat& key = bn.rotation_keys.data[k];
					int frame = (int)std::lround(key.time * fps);
					Quat q = engineQuat(key.value);
					if (conv_active) q = matrixQuat(conv_tform.m * Matrix(q) * -conv_tform.m);
					anim.setRotationKey(frame, q.normalized());
					if (frame > frames) frames = frame;
				}
				for (size_t k = 0; k < bn.scale_keys.count; ++k) {
					const ufbx_baked_vec3& key = bn.scale_keys.data[k];
					int frame = (int)std::lround(key.time * fps);
					anim.setScaleKey(frame, vec(key.value));
					if (frame > frames) frames = frame;
				}

				obj->setAnimation(anim);
			}

			ufbx_free_baked_anim(bake);
			return frames + 1;
		}

		void buildMesh(ufbx_node* node) {
			ufbx_mesh* mesh = node->mesh;
			std::map<const ufbx_node*, Entity*>::const_iterator it = objects.find(node);
			if (it == objects.end()) return;
			Model* obj = it->second->getModel();
			MeshModel* model = obj ? obj->getMeshModel() : 0;
			if (!model) return;

			bool skinned = !collapse && mesh->skin_deformers.count > 0;
			Transform bind_world;
			if (skinned) {
				ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
				if (skin->clusters.count) {
					ufbx_matrix m = ufbx_matrix_mul(&skin->clusters.data[0]->bind_to_world, &skin->clusters.data[0]->geometry_to_bone);
					bind_world = convTform(matrixTform(m));
				}
				else {
					bind_world = convTform(matrixTform(node->geometry_to_world));
				}
			}

			MeshLoader::beginMesh();

			for (size_t i = 0; i < mesh->num_indices; ++i) {
				Surface::Vertex v;
				Vector p = vec(ufbx_get_vertex_vec3(&mesh->vertex_position, i));
				Vector n(0, 0, 1);
				if (mesh->vertex_normal.exists) n = vec(ufbx_get_vertex_vec3(&mesh->vertex_normal, i));

				if (skinned) {
					p = bind_world * p;
					n = bind_world.m.cofactor() * n;
				}
				v.coords = convVec(p);
				v.normal = convNrm(n);

				if (mesh->vertex_uv.exists) {
					ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, i);
					v.tex_coords[0][0] = v.tex_coords[1][0] = uv.x;
					v.tex_coords[0][1] = v.tex_coords[1][1] = 1.0f - uv.y;
				}
				if (mesh->vertex_color.exists) {
					ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh->vertex_color, i);
					int a = (int)(std::clamp((float)c.w, 0.0f, 1.0f) * 255);
					int r = (int)(std::clamp((float)c.x, 0.0f, 1.0f) * 255);
					int g = (int)(std::clamp((float)c.y, 0.0f, 1.0f) * 255);
					int b = (int)(std::clamp((float)c.z, 0.0f, 1.0f) * 255);
					v.color = (a << 24) | (r << 16) | (g << 8) | b;
				}

				MeshLoader::addVertex(v);
			}

			if (skinned) {
				ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
				std::vector<int> cluster_bone(skin->clusters.count, -1);
				for (size_t k = 0; k < skin->clusters.count; ++k) {
					cluster_bone[k] = boneIndex(skin->clusters.data[k]->bone_node);
				}

				for (size_t i = 0; i < mesh->num_indices; ++i) {
					uint32_t vi = i < mesh->vertex_indices.count ? mesh->vertex_indices.data[i] : (uint32_t)i;
					if (vi >= skin->vertices.count) continue;
					const ufbx_skin_vertex& sv = skin->vertices.data[vi];
					for (uint32_t k = 0; k < sv.num_weights; ++k) {
						size_t w = (size_t)sv.weight_begin + k;
						if (w >= skin->weights.count) break;
						const ufbx_skin_weight& sw = skin->weights.data[w];
						if (sw.cluster_index >= cluster_bone.size()) continue;
						int bone = cluster_bone[sw.cluster_index];
						if (bone < 0 || bone > 255) continue;
						MeshLoader::addBone((int)i, sw.weight, bone);
					}
				}
			}

			std::vector<uint32_t> tri_indices;
			for (size_t i = 0; i < mesh->material_parts.count; ++i) {
				const ufbx_mesh_part& part = mesh->material_parts.data[i];
				const ufbx_material* mat = 0;
				if (part.index < node->materials.count) mat = node->materials.data[part.index];
				else if (part.index < mesh->materials.count) mat = mesh->materials.data[part.index];
				Brush brush = makeBrush(mat);

				for (size_t k = 0; k < part.face_indices.count; ++k) {
					uint32_t fi = part.face_indices.data[k];
					if (fi >= mesh->faces.count) continue;
					ufbx_face face = mesh->faces.data[fi];

					size_t capacity = (size_t)mesh->max_face_triangles * 3;
					if (capacity < 3) continue;
					tri_indices.resize(capacity);
					uint32_t tris = ufbx_triangulate_face(tri_indices.data(), capacity, mesh, face);
					for (uint32_t t = 0; t < tris; ++t) {
						addTri((int)tri_indices[t * 3], (int)tri_indices[t * 3 + 1], (int)tri_indices[t * 3 + 2], brush);
					}
				}
			}

			MeshLoader::endMesh(model);

			if (!mesh->vertex_normal.exists) model->updateNormals();
			if (skinned) {
				if (node->children.count == 0) {
					model->setParent(root);
					model->setLocalTform(Transform());
				}
				model->createBones(anim_objects);
			}
		}

		MeshModel* build() {
			root = new MeshModel();
			makeObjects();

			bindPose();

			collect(root, anim_objects);
			for (size_t i = 0; i < anim_objects.size(); ++i) anim_index[anim_objects[i]] = (int)i;

			int frames = 0;
			if (!collapse) frames = bakeAnimation();

			bool skinned = false;
			if (!collapse && !animonly) {
				for (size_t i = 0; i < scene->meshes.count; ++i) {
					if (scene->meshes.data[i]->skin_deformers.count) {
						skinned = true;
						break;
					}
				}
			}
			if (frames > 0 || skinned) {
				root->setAnimator(new Animator(anim_objects, frames > 0 ? frames : 1));
			}

			if (!animonly) {
				for (size_t i = 0; i < scene->nodes.count; ++i) {
					ufbx_node* node = scene->nodes.data[i];
					if (node != scene->root_node && node->mesh) buildMesh(node);
				}
			}

			return root;
		}
	};
}

MeshModel* Loader_FBX::load(const std::string& f, const Transform& conv, int hint) {

	conv_tform = conv;
	conv_active = conv != Transform();
	flip_tris = conv_active && conv.m.determinant() < 0;

	ufbx_load_opts opts = { 0 };
	opts.target_axes = ufbx_axes_left_handed_y_up;
	opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;
	opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
	opts.target_unit_meters = 1.0;
	opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
	opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
	opts.generate_missing_normals = true;
	opts.clean_skin_weights = true;

	ufbx_error error;
	ufbx_scene* scene = ufbx_load_file(f.c_str(), &opts, &error);
	if (!scene) return 0;

	Builder builder;
	builder.scene = scene;
	builder.path = filenamepath(f);
	builder.collapse = (hint & MeshLoader::HINT_COLLAPSE) != 0;
	builder.animonly = (hint & MeshLoader::HINT_ANIMONLY) != 0;

	MeshModel* root = builder.build();

	ufbx_free_scene(scene);
	return root;
}
