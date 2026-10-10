#ifndef SCENE_H
#define SCENE_H

#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "entity.h"

class sdlLight;
class Mirror;
class Listener;

struct CollInfo {
    int dst_type, method, response;
};

class Scene {
public:
    int id;
    bool active = true;
    std::unordered_set<Entity*> members;

    std::vector<sdlLight*> lights;
    std::vector<Mirror*> mirrors;
    std::vector<Listener*> listeners;
    std::unordered_map<int, std::vector<CollInfo>> collisions;

    Scene(int sceneId) : id(sceneId) {}
    ~Scene() {}

    void add(Entity* e) { members.insert(e); }
    void remove(Entity* e) { members.erase(e); }
};

class SceneManager {
public:
    std::unordered_map<int, Scene*> scenes;
    int nextId = 1;
    int currentSceneId = 0;

    SceneManager() {
        scenes[0] = new Scene(0); // default global scene
        nextId = 1;
    }
    ~SceneManager() { clearAll(); }

    int createScene() {
        int id = nextId++;
        scenes[id] = new Scene(id);
        return id;
    }

    Scene* get(int id) {
        auto it = scenes.find(id);
        if (it != scenes.end()) return it->second;
        if (id == 0) {
            scenes[0] = new Scene(0);
            return scenes[0];
        }
        return nullptr;
    }

    void setCurrent(int id) {
        if (id == 0 || scenes.find(id) != scenes.end()) {
            currentSceneId = id;
        }
    }

    void clearScene(int id) {
        Scene* s = get(id);
        if (!s) return;
        while (!s->members.empty()) {
            Entity* e = *s->members.begin();
            delete e;
        }
        delete s;
        scenes.erase(id);
        if (currentSceneId == id) currentSceneId = 0;
    }

    void clearAll() {
        for (auto& pair : scenes) {
            if (pair.first == 0) continue; // never delete global scene
            delete pair.second;
        }
        scenes.clear();
        scenes[0] = new Scene(0);
        currentSceneId = 0;
    }
};

extern SceneManager g_sceneManager;

#endif