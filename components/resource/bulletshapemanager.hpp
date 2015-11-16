#ifndef OPENMW_COMPONENTS_BULLETSHAPEMANAGER_H
#define OPENMW_COMPONENTS_BULLETSHAPEMANAGER_H

#include <map>
#include <string>

#include <osg/ref_ptr>

#include "bulletshape.hpp"

namespace VFS
{
    class Manager;
}

namespace Resource
{
    class SceneManager;

    class BulletShape;
    class BulletShapeInstance;

    class BulletShapeManager
    {
    public:
        BulletShapeManager(const VFS::Manager* vfs, SceneManager* sceneMgr);
        ~BulletShapeManager();

        osg::ref_ptr<BulletShapeInstance> createInstance(const std::string& name);

    private:
        const VFS::Manager* mVFS;
        SceneManager* mSceneManager;

        typedef std::map<std::string, osg::ref_ptr<BulletShape> > Index;
        Index mIndex;
    };

}

#endif
