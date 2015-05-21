#include "physicssystem.hpp"

#include <stdexcept>

#include <osg/Group>
#include <osg/PositionAttitudeTransform>

#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>

#include <components/nifbullet/bulletshapemanager.hpp>
#include <components/nifbullet/bulletnifloader.hpp>
#include <components/resource/resourcesystem.hpp>

#include <components/esm/loadgmst.hpp>

#include <components/nifosg/particle.hpp> // FindRecIndexVisitor

#include "../mwbase/world.hpp"
#include "../mwbase/environment.hpp"

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/cellstore.hpp"

#include "../mwrender/bulletdebugdraw.hpp"

#include "../mwbase/world.hpp"
#include "../mwbase/environment.hpp"

#include "../mwworld/class.hpp"

#include "collisiontype.hpp"
#include "actor.hpp"
#include "convert.hpp"
#include "trace.h"

namespace MWPhysics
{

    static const float sMaxSlope = 49.0f;
    static const float sStepSizeUp = 34.0f;
    static const float sStepSizeDown = 62.0f;

    // Arbitrary number. To prevent infinite loops. They shouldn't happen but it's good to be prepared.
    static const int sMaxIterations = 8;

    // FIXME: move to a separate file
    class MovementSolver
    {
    private:
        static float getSlope(const osg::Vec3f &normal)
        {
            return osg::RadiansToDegrees(std::acos(normal * osg::Vec3f(0.f, 0.f, 1.f)));
        }

        static bool stepMove(btCollisionObject *colobj, osg::Vec3f &position,
                             const osg::Vec3f &toMove, float &remainingTime, btDynamicsWorld* dynamicsWorld)
        {
            /*
             * Slide up an incline or set of stairs.  Should be called only after a
             * collision detection otherwise unnecessary tracing will be performed.
             *
             * NOTE: with a small change this method can be used to step over an obstacle
             * of height sStepSize.
             *
             * If successful return 'true' and update 'position' to the new possible
             * location and adjust 'remainingTime'.
             *
             * If not successful return 'false'.  May fail for these reasons:
             *    - can't move directly up from current position
             *    - having moved up by between epsilon() and sStepSize, can't move forward
             *    - having moved forward by between epsilon() and toMove,
             *        = moved down between 0 and just under sStepSize but slope was too steep, or
             *        = moved the full sStepSize down (FIXME: this could be a bug)
             *
             *
             *
             * Starting position.  Obstacle or stairs with height upto sStepSize in front.
             *
             *     +--+                          +--+       |XX
             *     |  | -------> toMove          |  |    +--+XX
             *     |  |                          |  |    |XXXXX
             *     |  | +--+                     |  | +--+XXXXX
             *     |  | |XX|                     |  | |XXXXXXXX
             *     +--+ +--+                     +--+ +--------
             *    ==============================================
             */

            /*
             * Try moving up sStepSize using stepper.
             * FIXME: does not work in case there is no front obstacle but there is one above
             *
             *     +--+                         +--+
             *     |  |                         |  |
             *     |  |                         |  |       |XX
             *     |  |                         |  |    +--+XX
             *     |  |                         |  |    |XXXXX
             *     +--+ +--+                    +--+ +--+XXXXX
             *          |XX|                         |XXXXXXXX
             *          +--+                         +--------
             *    ==============================================
             */
            ActorTracer tracer, stepper;

            stepper.doTrace(colobj, position, position+osg::Vec3f(0.0f,0.0f,sStepSizeUp), dynamicsWorld);
            if(stepper.mFraction < std::numeric_limits<float>::epsilon())
                return false; // didn't even move the smallest representable amount
                              // (TODO: shouldn't this be larger? Why bother with such a small amount?)

            /*
             * Try moving from the elevated position using tracer.
             *
             *                          +--+  +--+
             *                          |  |  |YY|   FIXME: collision with object YY
             *                          |  |  +--+
             *                          |  |
             *     <------------------->|  |
             *          +--+            +--+
             *          |XX|      the moved amount is toMove*tracer.mFraction
             *          +--+
             *    ==============================================
             */
            tracer.doTrace(colobj, stepper.mEndPos, stepper.mEndPos + toMove, dynamicsWorld);
            if(tracer.mFraction < std::numeric_limits<float>::epsilon())
                return false; // didn't even move the smallest representable amount

            /*
             * Try moving back down sStepSizeDown using stepper.
             * NOTE: if there is an obstacle below (e.g. stairs), we'll be "stepping up".
             * Below diagram is the case where we "stepped over" an obstacle in front.
             *
             *                                +--+
             *                                |YY|
             *                          +--+  +--+
             *                          |  |
             *                          |  |
             *          +--+            |  |
             *          |XX|            |  |
             *          +--+            +--+
             *    ==============================================
             */
            stepper.doTrace(colobj, tracer.mEndPos, tracer.mEndPos-osg::Vec3f(0.0f,0.0f,sStepSizeDown), dynamicsWorld);
            if(stepper.mFraction < 1.0f && getSlope(stepper.mPlaneNormal) <= sMaxSlope)
            {
                // don't allow stepping up other actors
                if (stepper.mHitObject->getBroadphaseHandle()->m_collisionFilterGroup == CollisionType_Actor)
                    return false;
                // only step down onto semi-horizontal surfaces. don't step down onto the side of a house or a wall.
                // TODO: stepper.mPlaneNormal does not appear to be reliable - needs more testing
                // NOTE: caller's variables 'position' & 'remainingTime' are modified here
                position = stepper.mEndPos;
                remainingTime *= (1.0f-tracer.mFraction); // remaining time is proportional to remaining distance
                return true;
            }

            // moved between 0 and just under sStepSize distance but slope was too great,
            // or moved full sStepSize distance (FIXME: is this a bug?)
            return false;
        }


        ///Project a vector u on another vector v
        static inline osg::Vec3f project(const osg::Vec3f& u, const osg::Vec3f &v)
        {
            return v * (u * v);
            //            ^ dot product
        }

        ///Helper for computing the character sliding
        static inline osg::Vec3f slide(const osg::Vec3f& direction, const osg::Vec3f &planeNormal)
        {
            return direction - project(direction, planeNormal);
        }

        static inline osg::Vec3f reflect(const osg::Vec3& velocity, const osg::Vec3f& normal)
        {
            return velocity - (normal * (normal * velocity)) * 2;
            //                                  ^ dot product
        }


    public:
        static osg::Vec3f traceDown(const MWWorld::Ptr &ptr, Actor* actor, btDynamicsWorld* dynamicsWorld, float maxHeight)
        {
            osg::Vec3f position(ptr.getRefData().getPosition().asVec3());

            ActorTracer tracer;
            tracer.findGround(actor, position, position-osg::Vec3f(0,0,maxHeight), dynamicsWorld);
            if(tracer.mFraction >= 1.0f)
            {
                actor->setOnGround(false);
                return position;
            }
            else
            {
                // Check if we actually found a valid spawn point (use an infinitely thin ray this time).
                // Required for some broken door destinations in Morrowind.esm, where the spawn point
                // intersects with other geometry if the actor's base is taken into account
                btVector3 from = toBullet(position);
                btVector3 to = from - btVector3(0,0,maxHeight);

                btCollisionWorld::ClosestRayResultCallback resultCallback1(from, to);
                resultCallback1.m_collisionFilterGroup = 0xff;
                resultCallback1.m_collisionFilterMask = CollisionType_World|CollisionType_HeightMap;

                dynamicsWorld->rayTest(from, to, resultCallback1);
                if (resultCallback1.hasHit() &&
                        ( (toOsg(resultCallback1.m_hitPointWorld) - tracer.mEndPos).length() > 30
                        || getSlope(tracer.mPlaneNormal) > sMaxSlope))
                {
                    actor->setOnGround(getSlope(toOsg(resultCallback1.m_hitNormalWorld)) <= sMaxSlope);
                    return toOsg(resultCallback1.m_hitPointWorld) + osg::Vec3f(0.f, 0.f, 1.f);
                }

                actor->setOnGround(getSlope(tracer.mPlaneNormal) <= sMaxSlope);

                return tracer.mEndPos;
            }
        }

        static osg::Vec3f move(const MWWorld::Ptr &ptr, Actor* physicActor, const osg::Vec3f &movement, float time,
                                  bool isFlying, float waterlevel, float slowFall, btDynamicsWorld* dynamicsWorld
                                  , std::map<std::string, std::string>& collisionTracker
                                  , std::map<std::string, std::string>& standingCollisionTracker)
        {
            const ESM::Position& refpos = ptr.getRefData().getPosition();
            osg::Vec3f position(refpos.asVec3());

            // Early-out for totally static creatures
            // (Not sure if gravity should still apply?)
            if (!ptr.getClass().isMobile(ptr))
                return position;

            // Reset per-frame data
            physicActor->setWalkingOnWater(false);
            // Anything to collide with?
            if(!physicActor->getCollisionMode())
            {
                return position +  (osg::Quat(refpos.rot[0], osg::Vec3f(-1, 0, 0)) *
                                    osg::Quat(refpos.rot[2], osg::Vec3f(0, 0, -1))
                                    ) * movement * time;
            }

            btCollisionObject *colobj = physicActor->getCollisionObject();
            osg::Vec3f halfExtents = physicActor->getHalfExtents();
            position.z() += halfExtents.z();

            static const float fSwimHeightScale = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>()
                    .find("fSwimHeightScale")->getFloat();
            float swimlevel = waterlevel + halfExtents.z() - (halfExtents.z() * 2 * fSwimHeightScale);

            ActorTracer tracer;
            osg::Vec3f inertia = physicActor->getInertialForce();
            osg::Vec3f velocity;

            if(position.z() < swimlevel || isFlying)
            {
                velocity = (osg::Quat(refpos.rot[0], osg::Vec3f(-1, 0, 0)) *
                            osg::Quat(refpos.rot[2], osg::Vec3f(0, 0, -1))) * movement;
            }
            else
            {
                velocity = (osg::Quat(refpos.rot[2], osg::Vec3f(0, 0, -1))) * movement;

                if (velocity.z() > 0.f)
                    inertia = velocity;
                if(!physicActor->getOnGround())
                {
                    velocity = velocity + physicActor->getInertialForce();
                }
            }
            ptr.getClass().getMovementSettings(ptr).mPosition[2] = 0;

            // Now that we have the effective movement vector, apply wind forces to it
            if (MWBase::Environment::get().getWorld()->isInStorm())
            {
                osg::Vec3f stormDirection = MWBase::Environment::get().getWorld()->getStormDirection();
                float angleDegrees = osg::RadiansToDegrees(std::acos(stormDirection * velocity));
                static const float fStromWalkMult = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>()
                        .find("fStromWalkMult")->getFloat();
                velocity *= 1.f-(fStromWalkMult * (angleDegrees/180.f));
            }

            osg::Vec3f origVelocity = velocity;

            osg::Vec3f newPosition = position;
            /*
             * A loop to find newPosition using tracer, if successful different from the starting position.
             * nextpos is the local variable used to find potential newPosition, using velocity and remainingTime
             * The initial velocity was set earlier (see above).
             */
            float remainingTime = time;
            for(int iterations = 0; iterations < sMaxIterations && remainingTime > 0.01f; ++iterations)
            {
                osg::Vec3f nextpos = newPosition + velocity * remainingTime;

                // If not able to fly, don't allow to swim up into the air
                if(newPosition.z() < swimlevel &&
                   !isFlying &&  // can't fly
                   nextpos.z() > swimlevel &&     // but about to go above water
                   newPosition.z() <= swimlevel)
                {
                    const osg::Vec3f down(0,0,-1);
                    float movelen = velocity.normalize();
                    osg::Vec3f reflectdir = reflect(velocity, down);
                    reflectdir.normalize();
                    velocity = slide(reflectdir, down)*movelen;
                    // NOTE: remainingTime is unchanged before the loop continues
                    continue; // velocity updated, calculate nextpos again
                }

                if((newPosition - nextpos).length2() > 0.0001)
                {
                    // trace to where character would go if there were no obstructions
                    tracer.doTrace(colobj, newPosition, nextpos, dynamicsWorld);

                    // check for obstructions
                    if(tracer.mFraction >= 1.0f)
                    {
                        newPosition = tracer.mEndPos; // ok to move, so set newPosition
                        break;
                    }
                    else
                    {
                        /*
                        const btCollisionObject* standingOn = tracer.mHitObject;
                        if (const OEngine::Physic::RigidBody* body = dynamic_cast<const OEngine::Physic::RigidBody*>(standingOn))
                        {
                            collisionTracker[ptr.getRefData().getHandle()] = body->mName;
                        }
                        */
                    }
                }
                else
                {
                    // The current position and next position are nearly the same, so just exit.
                    // Note: Bullet can trigger an assert in debug modes if the positions
                    // are the same, since that causes it to attempt to normalize a zero
                    // length vector (which can also happen with nearly identical vectors, since
                    // precision can be lost due to any math Bullet does internally). Since we
                    // aren't performing any collision detection, we want to reject the next
                    // position, so that we don't slowly move inside another object.
                    break;
                }


                osg::Vec3f oldPosition = newPosition;
                // We hit something. Try to step up onto it. (NOTE: stepMove does not allow stepping over)
                // NOTE: stepMove modifies newPosition if successful
                bool result = stepMove(colobj, newPosition, velocity*remainingTime, remainingTime, dynamicsWorld);
                if (!result) // to make sure the maximum stepping distance isn't framerate-dependent or movement-speed dependent
                {
                    osg::Vec3f normalizedVelocity = velocity;
                    normalizedVelocity.normalize();
                    result = stepMove(colobj, newPosition, normalizedVelocity*10.f, remainingTime, dynamicsWorld);
                }
                if(result)
                {
                    // don't let pure water creatures move out of water after stepMove
                    if (ptr.getClass().isPureWaterCreature(ptr)
                            && newPosition.z() + halfExtents.z() > waterlevel)
                        newPosition = oldPosition;
                }
                else
                {
                    // Can't move this way, try to find another spot along the plane
                    osg::Vec3f direction = velocity;
                    float movelen = direction.normalize();
                    osg::Vec3f reflectdir = reflect(velocity, tracer.mPlaneNormal);
                    reflectdir.normalize();

                    osg::Vec3f newVelocity = slide(reflectdir, tracer.mPlaneNormal)*movelen;
                    if ((newVelocity-velocity).length2() < 0.01)
                        break;
                    if ((velocity * origVelocity) <= 0.f)
                        break; // ^ dot product

                    velocity = newVelocity;

                    // Do not allow sliding upward if there is gravity. Stepping will have taken
                    // care of that.
                    if(!(newPosition.z() < swimlevel || isFlying))
                        velocity.z() = std::min(velocity.z(), 0.0f);
                }
            }

            bool isOnGround = false;
            if (!(inertia.z() > 0.f) && !(newPosition.z() < swimlevel))
            {
                osg::Vec3f from = newPosition;
                osg::Vec3f to = newPosition - (physicActor->getOnGround() ?
                             osg::Vec3f(0,0,sStepSizeDown+2.f) : osg::Vec3f(0,0,2.f));
                tracer.doTrace(colobj, from, to, dynamicsWorld);
                if(tracer.mFraction < 1.0f && getSlope(tracer.mPlaneNormal) <= sMaxSlope
                        && tracer.mHitObject->getBroadphaseHandle()->m_collisionFilterGroup != CollisionType_Actor)
                {
                    /*
                    const btCollisionObject* standingOn = tracer.mHitObject;

                    if (const OEngine::Physic::RigidBody* body = dynamic_cast<const OEngine::Physic::RigidBody*>(standingOn))
                    {
                        standingCollisionTracker[ptr.getRefData().getHandle()] = body->mName;
                    }
                    if (standingOn->getBroadphaseHandle()->m_collisionFilterGroup == CollisionType_Water)
                        physicActor->setWalkingOnWater(true);
                    */
                    if (!isFlying)
                        newPosition.z() = tracer.mEndPos.z() + 1.0f;

                    isOnGround = true;
                }
                else
                {
                    // standing on actors is not allowed (see above).
                    // in addition to that, apply a sliding effect away from the center of the actor,
                    // so that we do not stay suspended in air indefinitely.
                    if (tracer.mFraction < 1.0f && tracer.mHitObject->getBroadphaseHandle()->m_collisionFilterGroup == CollisionType_Actor)
                    {
                        if (osg::Vec3f(velocity.x(), velocity.y(), 0).length2() < 100.f*100.f)
                        {
                            btVector3 aabbMin, aabbMax;
                            tracer.mHitObject->getCollisionShape()->getAabb(tracer.mHitObject->getWorldTransform(), aabbMin, aabbMax);
                            btVector3 center = (aabbMin + aabbMax) / 2.f;
                            inertia = osg::Vec3f(position.x() - center.x(), position.y() - center.y(), 0);
                            inertia.normalize();
                            inertia *= 100;
                        }
                    }

                    isOnGround = false;
                }
            }

            if(isOnGround || newPosition.z() < swimlevel || isFlying)
                physicActor->setInertialForce(osg::Vec3f(0.f, 0.f, 0.f));
            else
            {
                inertia.z() += time * -627.2f;
                if (inertia.z() < 0)
                    inertia.z() *= slowFall;
                physicActor->setInertialForce(inertia);
            }
            physicActor->setOnGround(isOnGround);

            newPosition.z() -= halfExtents.z(); // remove what was added at the beginning
            return newPosition;
        }
    };


    // ---------------------------------------------------------------

    class HeightField
    {
    public:
        HeightField(float* heights, int x, int y, float triSize, float sqrtVerts)
        {
            // find the minimum and maximum heights (needed for bullet)
            float minh = heights[0];
            float maxh = heights[0];
            for(int i = 1;i < sqrtVerts*sqrtVerts;++i)
            {
                float h = heights[i];
                if(h > maxh) maxh = h;
                if(h < minh) minh = h;
            }

            mShape = new btHeightfieldTerrainShape(
                sqrtVerts, sqrtVerts, heights, 1,
                minh, maxh, 2,
                PHY_FLOAT, true
            );
            mShape->setUseDiamondSubdivision(true);
            mShape->setLocalScaling(btVector3(triSize, triSize, 1));

            btTransform transform(btQuaternion::getIdentity(),
                                  btVector3((x+0.5f) * triSize * (sqrtVerts-1),
                                            (y+0.5f) * triSize * (sqrtVerts-1),
                                            (maxh+minh)*0.5f));

            mCollisionObject = new btCollisionObject;
            mCollisionObject->setCollisionShape(mShape);
            mCollisionObject->setWorldTransform(transform);
        }
        ~HeightField()
        {
            delete mCollisionObject;
            delete mShape;
        }
        btCollisionObject* getCollisionObject()
        {
            return mCollisionObject;
        }

    private:
        btHeightfieldTerrainShape* mShape;
        btCollisionObject* mCollisionObject;
    };

    // --------------------------------------------------------------

    class Object : public PtrHolder
    {
    public:
        Object(const MWWorld::Ptr& ptr, osg::ref_ptr<NifBullet::BulletShapeInstance> shapeInstance)
            : mShapeInstance(shapeInstance)
        {
            mPtr = ptr;

            mCollisionObject.reset(new btCollisionObject);
            mCollisionObject->setCollisionShape(shapeInstance->getCollisionShape());

            mCollisionObject->setUserPointer(static_cast<PtrHolder*>(this));

            setScale(ptr.getCellRef().getScale());
            setRotation(toBullet(ptr.getRefData().getBaseNode()->getAttitude()));
            const float* pos = ptr.getRefData().getPosition().pos;
            setOrigin(btVector3(pos[0], pos[1], pos[2]));
        }

        void setScale(float scale)
        {
            mShapeInstance->getCollisionShape()->setLocalScaling(btVector3(scale,scale,scale));
        }

        void setRotation(const btQuaternion& quat)
        {
            mCollisionObject->getWorldTransform().setRotation(quat);
        }

        void setOrigin(const btVector3& vec)
        {
            mCollisionObject->getWorldTransform().setOrigin(vec);
        }

        btCollisionObject* getCollisionObject()
        {
            return mCollisionObject.get();
        }

        void animateCollisionShapes(btDynamicsWorld* dynamicsWorld)
        {
            if (mShapeInstance->mAnimatedShapes.empty())
                return;

            assert (mShapeInstance->getCollisionShape()->isCompound());

            btCompoundShape* compound = dynamic_cast<btCompoundShape*>(mShapeInstance->getCollisionShape());

            for (std::map<int, int>::const_iterator it = mShapeInstance->mAnimatedShapes.begin(); it != mShapeInstance->mAnimatedShapes.end(); ++it)
            {
                int recIndex = it->first;
                int shapeIndex = it->second;

                NifOsg::FindRecIndexVisitor visitor(recIndex);
                mPtr.getRefData().getBaseNode()->accept(visitor);
                if (!visitor.mFound)
                {
                    std::cerr << "animateCollisionShapes: Can't find node " << recIndex << std::endl;
                    return;
                }

                osg::NodePath path = visitor.mFoundPath;
                path.erase(path.begin());
                osg::Matrixf matrix = osg::computeLocalToWorld(path);
                osg::Vec3f scale = matrix.getScale();
                matrix.orthoNormalize(matrix);

                btTransform transform;
                transform.setOrigin(toBullet(matrix.getTrans()) * compound->getLocalScaling());
                for (int i=0; i<3; ++i)
                    for (int j=0; j<3; ++j)
                        transform.getBasis()[i][j] = matrix(j,i); // NB column/row major difference

                compound->getChildShape(shapeIndex)->setLocalScaling(compound->getLocalScaling() * toBullet(scale));
                compound->updateChildTransform(shapeIndex, transform);
            }

            dynamicsWorld->updateSingleAabb(mCollisionObject.get());
        }

    private:
        std::auto_ptr<btCollisionObject> mCollisionObject;
        osg::ref_ptr<NifBullet::BulletShapeInstance> mShapeInstance;
    };

    // ---------------------------------------------------------------

    PhysicsSystem::PhysicsSystem(Resource::ResourceSystem* resourceSystem, osg::ref_ptr<osg::Group> parentNode)
        : mShapeManager(new NifBullet::BulletShapeManager(resourceSystem->getVFS(), resourceSystem->getSceneManager()))
        , mTimeAccum(0.0f)
        , mWaterEnabled(false)
        , mWaterHeight(0)
        , mDebugDrawEnabled(false)
        , mParentNode(parentNode)
    {
        mCollisionConfiguration = new btDefaultCollisionConfiguration();
        mDispatcher = new btCollisionDispatcher(mCollisionConfiguration);
        mSolver = new btSequentialImpulseConstraintSolver;
        mBroadphase = new btDbvtBroadphase();
        mDynamicsWorld = new btDiscreteDynamicsWorld(mDispatcher,mBroadphase,mSolver,mCollisionConfiguration);

        // Don't update AABBs of all objects every frame. Most objects in MW are static, so we don't need this.
        // Should a "static" object ever be moved, we have to update its AABB manually using DynamicsWorld::updateSingleAabb.
        mDynamicsWorld->setForceUpdateAllAabbs(false);

        mDynamicsWorld->setGravity(btVector3(0,0,-10));
    }

    PhysicsSystem::~PhysicsSystem()
    {
        if (mWaterCollisionObject.get())
            mDynamicsWorld->removeCollisionObject(mWaterCollisionObject.get());

        for (HeightFieldMap::iterator it = mHeightFields.begin(); it != mHeightFields.end(); ++it)
        {
            mDynamicsWorld->removeCollisionObject(it->second->getCollisionObject());
            delete it->second;
        }

        for (ObjectMap::iterator it = mObjects.begin(); it != mObjects.end(); ++it)
        {
            mDynamicsWorld->removeCollisionObject(it->second->getCollisionObject());
            delete it->second;
        }

        for (ActorMap::iterator it = mActors.begin(); it != mActors.end(); ++it)
        {
            delete it->second;
        }

        delete mDynamicsWorld;
        delete mSolver;
        delete mCollisionConfiguration;
        delete mDispatcher;
        delete mBroadphase;
    }

    bool PhysicsSystem::toggleDebugRendering()
    {
        mDebugDrawEnabled = !mDebugDrawEnabled;

        if (mDebugDrawEnabled && !mDebugDrawer.get())
        {
            mDebugDrawer.reset(new MWRender::DebugDrawer(mParentNode, mDynamicsWorld));
            mDynamicsWorld->setDebugDrawer(mDebugDrawer.get());
            mDebugDrawer->setDebugMode(mDebugDrawEnabled);
        }
        else if (mDebugDrawer.get())
            mDebugDrawer->setDebugMode(mDebugDrawEnabled);
        return mDebugDrawEnabled;
    }

    std::pair<std::string,Ogre::Vector3> PhysicsSystem::getHitContact(const std::string &name,
                                                                      const Ogre::Vector3 &origin,
                                                                      const Ogre::Quaternion &orient,
                                                                      float queryDistance)
    {
        return std::make_pair(std::string(), Ogre::Vector3());
        /*
        const MWWorld::Store<ESM::GameSetting> &store = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>();

        btConeShape shape(Ogre::Degree(store.find("fCombatAngleXY")->getFloat()/2.0f).valueRadians(),
                          queryDistance);
        shape.setLocalScaling(btVector3(1, 1, Ogre::Degree(store.find("fCombatAngleZ")->getFloat()/2.0f).valueRadians() /
                                              shape.getRadius()));

        // The shape origin is its center, so we have to move it forward by half the length. The
        // real origin will be provided to getFilteredContact to find the closest.
        Ogre::Vector3 center = origin + (orient * Ogre::Vector3(0.0f, queryDistance*0.5f, 0.0f));

        btCollisionObject object;
        object.setCollisionShape(&shape);
        object.setWorldTransform(btTransform(btQuaternion(orient.x, orient.y, orient.z, orient.w),
                                             btVector3(center.x, center.y, center.z)));

        std::pair<const OEngine::Physic::RigidBody*,btVector3> result = mEngine->getFilteredContact(
                name, btVector3(origin.x, origin.y, origin.z), &object);
        if(!result.first)
            return std::make_pair(std::string(), Ogre::Vector3(&result.second[0]));
        return std::make_pair(result.first->mName, Ogre::Vector3(&result.second[0]));
        */
    }


    bool PhysicsSystem::castRay(const Ogre::Vector3& from, const Ogre::Vector3& to, bool ignoreHeightMap)
    {
        return false;
        /*
        btVector3 _from, _to;
        _from = btVector3(from.x, from.y, from.z);
        _to = btVector3(to.x, to.y, to.z);

        std::pair<std::string, float> result = mEngine->rayTest(_from, _to,ignoreHeightMap);
        return !(result.first == "");
        */
    }

    std::pair<bool, Ogre::Vector3>
    PhysicsSystem::castRay(const Ogre::Vector3 &orig, const Ogre::Vector3 &dir, float len)
    {
        return std::make_pair(false, Ogre::Vector3());
        /*
        Ogre::Ray ray = Ogre::Ray(orig, dir);
        Ogre::Vector3 to = ray.getPoint(len);

        btVector3 btFrom = btVector3(orig.x, orig.y, orig.z);
        btVector3 btTo = btVector3(to.x, to.y, to.z);

        std::pair<std::string, float> test = mEngine->rayTest(btFrom, btTo);
        if (test.second == -1) {
            return std::make_pair(false, Ogre::Vector3());
        }
        return std::make_pair(true, ray.getPoint(len * test.second));
        */
    }

    std::vector<std::string> PhysicsSystem::getCollisions(const MWWorld::Ptr &ptr, int collisionGroup, int collisionMask)
    {
        return std::vector<std::string>();//mEngine->getCollisions(ptr.getRefData().getBaseNodeOld()->getName(), collisionGroup, collisionMask);
    }

    osg::Vec3f PhysicsSystem::traceDown(const MWWorld::Ptr &ptr, float maxHeight)
    {
        ActorMap::iterator found = mActors.find(ptr);
        if (found ==  mActors.end())
            return ptr.getRefData().getPosition().asVec3();
        else
            return MovementSolver::traceDown(ptr, found->second, mDynamicsWorld, maxHeight);
    }

    void PhysicsSystem::addHeightField (float* heights, int x, int y, float triSize, float sqrtVerts)
    {
        HeightField *heightfield = new HeightField(heights, x, y, triSize, sqrtVerts);
        mHeightFields[std::make_pair(x,y)] = heightfield;

        mDynamicsWorld->addCollisionObject(heightfield->getCollisionObject(), CollisionType_HeightMap,
            CollisionType_Actor|CollisionType_Projectile);
    }

    void PhysicsSystem::removeHeightField (int x, int y)
    {
        HeightFieldMap::iterator heightfield = mHeightFields.find(std::make_pair(x,y));
        if(heightfield != mHeightFields.end())
        {
            mDynamicsWorld->removeCollisionObject(heightfield->second->getCollisionObject());
            delete heightfield->second;
            mHeightFields.erase(heightfield);
        }
    }

    void PhysicsSystem::addObject (const MWWorld::Ptr& ptr, const std::string& mesh)
    {
        osg::ref_ptr<NifBullet::BulletShapeInstance> shapeInstance = mShapeManager->createInstance(mesh);
        if (!shapeInstance->getCollisionShape())
            return;

        Object *obj = new Object(ptr, shapeInstance);
        mObjects.insert(std::make_pair(ptr, obj));

        mDynamicsWorld->addCollisionObject(obj->getCollisionObject(), CollisionType_World,
                                           CollisionType_Actor|CollisionType_HeightMap|CollisionType_Projectile);
    }

    void PhysicsSystem::remove(const MWWorld::Ptr &ptr)
    {
        ObjectMap::iterator found = mObjects.find(ptr);
        if (found != mObjects.end())
        {
            mDynamicsWorld->removeCollisionObject(found->second->getCollisionObject());
            delete found->second;
            mObjects.erase(found);
        }

        ActorMap::iterator foundActor = mActors.find(ptr);
        if (foundActor != mActors.end())
        {
            delete foundActor->second;
            mActors.erase(foundActor);
        }
    }

    void PhysicsSystem::updatePtr(const MWWorld::Ptr &old, const MWWorld::Ptr &updated)
    {
        ObjectMap::iterator found = mObjects.find(old);
        if (found != mObjects.end())
        {
            Object* obj = found->second;
            obj->updatePtr(updated);
            mObjects.erase(found);
            mObjects.insert(std::make_pair(updated, obj));
        }

        ActorMap::iterator foundActor = mActors.find(old);
        if (foundActor != mActors.end())
        {
            Actor* actor = foundActor->second;
            actor->updatePtr(updated);
            mActors.erase(foundActor);
            mActors.insert(std::make_pair(updated, actor));
        }
    }

    Actor *PhysicsSystem::getActor(const MWWorld::Ptr &ptr)
    {
        ActorMap::iterator found = mActors.find(ptr);
        if (found != mActors.end())
            return found->second;
        return NULL;
    }

    void PhysicsSystem::updateScale(const MWWorld::Ptr &ptr)
    {
        ObjectMap::iterator found = mObjects.find(ptr);
        float scale = ptr.getCellRef().getScale();
        if (found != mObjects.end())
        {
            found->second->setScale(scale);
            mDynamicsWorld->updateSingleAabb(found->second->getCollisionObject());
            return;
        }
        ActorMap::iterator foundActor = mActors.find(ptr);
        if (foundActor != mActors.end())
        {
            foundActor->second->updateScale();
            // no aabb update needed (DISABLE_DEACTIVATION)
            return;
        }
    }

    void PhysicsSystem::updateRotation(const MWWorld::Ptr &ptr)
    {
        ObjectMap::iterator found = mObjects.find(ptr);
        if (found != mObjects.end())
        {
            found->second->setRotation(toBullet(ptr.getRefData().getBaseNode()->getAttitude()));
            mDynamicsWorld->updateSingleAabb(found->second->getCollisionObject());
            return;
        }
        ActorMap::iterator foundActor = mActors.find(ptr);
        if (foundActor != mActors.end())
        {
            foundActor->second->updateRotation();
            // no aabb update needed (DISABLE_DEACTIVATION)
            return;
        }
    }

    void PhysicsSystem::updatePosition(const MWWorld::Ptr &ptr)
    {
        ObjectMap::iterator found = mObjects.find(ptr);
        if (found != mObjects.end())
        {
            found->second->setOrigin(toBullet(ptr.getRefData().getPosition().asVec3()));
            mDynamicsWorld->updateSingleAabb(found->second->getCollisionObject());
            return;
        }
        ActorMap::iterator foundActor = mActors.find(ptr);
        if (foundActor != mActors.end())
        {
            foundActor->second->updatePosition();
            // no aabb update needed (DISABLE_DEACTIVATION)
            return;
        }
    }

    void PhysicsSystem::addActor (const MWWorld::Ptr& ptr, const std::string& mesh)
    {
        osg::ref_ptr<NifBullet::BulletShapeInstance> shapeInstance = mShapeManager->createInstance(mesh);

        Actor* actor = new Actor(ptr, shapeInstance, mDynamicsWorld);
        mActors.insert(std::make_pair(ptr, actor));
    }

    bool PhysicsSystem::toggleCollisionMode()
    {
        ActorMap::iterator found = mActors.find(MWBase::Environment::get().getWorld()->getPlayerPtr());
        if (found != mActors.end())
        {
            bool cmode = found->second->getCollisionMode();
            cmode = !cmode;
            found->second->enableCollisionMode(cmode);
            return cmode;
        }

        return false;
    }

    void PhysicsSystem::queueObjectMovement(const MWWorld::Ptr &ptr, const osg::Vec3f &movement)
    {
        PtrVelocityList::iterator iter = mMovementQueue.begin();
        for(;iter != mMovementQueue.end();++iter)
        {
            if(iter->first == ptr)
            {
                iter->second = movement;
                return;
            }
        }

        mMovementQueue.push_back(std::make_pair(ptr, movement));
    }

    void PhysicsSystem::clearQueuedMovement()
    {
        mMovementQueue.clear();
        mCollisions.clear();
        mStandingCollisions.clear();
    }

    const PtrVelocityList& PhysicsSystem::applyQueuedMovement(float dt)
    {
        mMovementResults.clear();

        mTimeAccum += dt;
        if(mTimeAccum >= 1.0f/60.0f)
        {
            // Collision events should be available on every frame
            mCollisions.clear();
            mStandingCollisions.clear();

            const MWBase::World *world = MWBase::Environment::get().getWorld();
            PtrVelocityList::iterator iter = mMovementQueue.begin();
            for(;iter != mMovementQueue.end();++iter)
            {
                float waterlevel = -std::numeric_limits<float>::max();
                const MWWorld::CellStore *cell = iter->first.getCell();
                if(cell->getCell()->hasWater())
                    waterlevel = cell->getWaterLevel();

                float oldHeight = iter->first.getRefData().getPosition().pos[2];

                const MWMechanics::MagicEffects& effects = iter->first.getClass().getCreatureStats(iter->first).getMagicEffects();

                bool waterCollision = false;
                if (effects.get(ESM::MagicEffect::WaterWalking).getMagnitude()
                        && cell->getCell()->hasWater()
                        && !world->isUnderwater(iter->first.getCell(),
                                               Ogre::Vector3(iter->first.getRefData().getPosition().pos)))
                    waterCollision = true;

                ActorMap::iterator foundActor = mActors.find(iter->first);
                if (foundActor == mActors.end()) // actor was already removed from the scene
                    continue;
                Actor* physicActor = foundActor->second;
                physicActor->setCanWaterWalk(waterCollision);

                // Slow fall reduces fall speed by a factor of (effect magnitude / 200)
                float slowFall = 1.f - std::max(0.f, std::min(1.f, effects.get(ESM::MagicEffect::SlowFall).getMagnitude() * 0.005f));

                osg::Vec3f newpos = MovementSolver::move(iter->first, physicActor, iter->second, mTimeAccum,
                                                            world->isFlying(iter->first),
                                                            waterlevel, slowFall, mDynamicsWorld, mCollisions, mStandingCollisions);

                float heightDiff = newpos.z() - oldHeight;

                if (heightDiff < 0)
                    iter->first.getClass().getCreatureStats(iter->first).addToFallHeight(-heightDiff);

                mMovementResults.push_back(std::make_pair(iter->first, newpos));
            }

            mTimeAccum = 0.0f;
        }
        mMovementQueue.clear();

        return mMovementResults;
    }

    void PhysicsSystem::stepSimulation(float dt)
    {
        for (ObjectMap::iterator it = mObjects.begin(); it != mObjects.end(); ++it)
            it->second->animateCollisionShapes(mDynamicsWorld);

        // We have nothing to simulate, but character controllers aren't working without this call. Might be related to updating AABBs.
        mDynamicsWorld->stepSimulation(static_cast<btScalar>(dt), 1, 1 / 60.0f);

        if (mDebugDrawer.get())
            mDebugDrawer->step();
    }

    bool PhysicsSystem::isActorStandingOn(const MWWorld::Ptr &actor, const MWWorld::Ptr &object) const
    {
        /*
        const std::string& actorHandle = actor.getRefData().getHandle();
        const std::string& objectHandle = object.getRefData().getHandle();

        for (std::map<std::string, std::string>::const_iterator it = mStandingCollisions.begin();
             it != mStandingCollisions.end(); ++it)
        {
            if (it->first == actorHandle && it->second == objectHandle)
                return true;
        }
        */
        return false;
    }

    void PhysicsSystem::getActorsStandingOn(const MWWorld::Ptr &object, std::vector<std::string> &out) const
    {
        /*
        const std::string& objectHandle = object.getRefData().getHandle();

        for (std::map<std::string, std::string>::const_iterator it = mStandingCollisions.begin();
             it != mStandingCollisions.end(); ++it)
        {
            if (it->second == objectHandle)
                out.push_back(it->first);
        }
        */
    }

    bool PhysicsSystem::isActorCollidingWith(const MWWorld::Ptr &actor, const MWWorld::Ptr &object) const
    {
        /*
        const std::string& actorHandle = actor.getRefData().getHandle();
        const std::string& objectHandle = object.getRefData().getHandle();

        for (std::map<std::string, std::string>::const_iterator it = mCollisions.begin();
             it != mCollisions.end(); ++it)
        {
            if (it->first == actorHandle && it->second == objectHandle)
                return true;
        }
        */
        return false;
    }

    void PhysicsSystem::getActorsCollidingWith(const MWWorld::Ptr &object, std::vector<std::string> &out) const
    {
        /*
        const std::string& objectHandle = object.getRefData().getHandle();

        for (std::map<std::string, std::string>::const_iterator it = mCollisions.begin();
             it != mCollisions.end(); ++it)
        {
            if (it->second == objectHandle)
                out.push_back(it->first);
        }
        */
    }

    void PhysicsSystem::disableWater()
    {
        if (mWaterEnabled)
        {
            mWaterEnabled = false;
            updateWater();
        }
    }

    void PhysicsSystem::enableWater(float height)
    {
        if (!mWaterEnabled || mWaterHeight != height)
        {
            mWaterEnabled = true;
            mWaterHeight = height;
            updateWater();
        }
    }

    void PhysicsSystem::setWaterHeight(float height)
    {
        if (mWaterHeight != height)
        {
            mWaterHeight = height;
            updateWater();
        }
    }

    void PhysicsSystem::updateWater()
    {
        if (mWaterCollisionObject.get())
        {
            mDynamicsWorld->removeCollisionObject(mWaterCollisionObject.get());
        }

        if (!mWaterEnabled)
            return;

        mWaterCollisionObject.reset(new btCollisionObject());
        mWaterCollisionShape.reset(new btStaticPlaneShape(btVector3(0,0,1), mWaterHeight));
        mWaterCollisionObject->setCollisionShape(mWaterCollisionShape.get());
        mDynamicsWorld->addCollisionObject(mWaterCollisionObject.get(), CollisionType_Water,
                                                    CollisionType_Actor);
    }
}