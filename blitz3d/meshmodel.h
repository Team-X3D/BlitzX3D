#ifndef MESHMODEL_H
#define MESHMODEL_H

#include "model.h"
#include "surface.h"

class MeshCollider;

class MeshModel : public Model {
public:
	typedef std::vector<Surface*> SurfaceList;

	MeshModel();
	MeshModel(const MeshModel& t);
	~MeshModel();

	//Entity interface
	virtual MeshModel* getMeshModel() { return this; }
	virtual Entity* clone() { return new MeshModel(*this); }

	//Object interface
	virtual bool collide(const Line& line, float radius, Collision* curr_coll, const Transform& t);
	virtual Box getWorldBounds() const;

	//Model interface
	virtual void setRenderBrush(const Brush& b);
	virtual bool render(const RenderContext& rc);
	virtual void renderQueue(int type);

	//boned mesh!
	void createBones();

	//MeshModel interface
	Surface* createSurface(const Brush& b);
	void setCullBox(const Box& box);
	void updateNormals();
	void flipTriangles();
	void transform(const Transform& t);
	void paint(const Brush& b);
	void add(const MeshModel& t);

	//accessors
	const SurfaceList& getSurfaces()const;
	Surface* findSurface(const Brush& b)const;
	bool intersects(const MeshModel& m)const;
	MeshCollider* getCollider()const;
	const Box& getBox()const;

	//GPU SKINNING
	static void setGpuSkinningEnabled(bool enable) { gpu_skinning_enabled = enable; }
	static bool getGpuSkinningEnabled() { return gpu_skinning_enabled; }

private:
	struct Rep;

	Rep* rep;
	int brush_changes;
	Brush render_brush;
	std::vector<Brush> brushes;

	std::vector<Surface::Bone> surf_bones;

	void packBones(std::vector<float>& out)const;
	bool wantGpuSkinning();
	static bool gpu_skinning_enabled;
	bool warned_bone_overflow = false;
};

#endif