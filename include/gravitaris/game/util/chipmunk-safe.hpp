#pragma once

#include <memory>

#include <Magnum/Magnum.h>
#include <Magnum/Types.h>
#include <Magnum/Math/Vector2.h>

#include <chipmunk/chipmunk.h>

namespace Magnum {
namespace Math {
namespace Implementation {

template<> struct VectorConverter<2, Double, cpVect> {
    static Vector<2, Double> from(const cpVect& other) {
        return {other.x, other.y};
    }

    static cpVect to(const Vector<2, Double>& other) {
        return {other[0], other[1]};
    }
};

} // namespace Implementation
} // namespace Math
} // namespace Magnum

namespace Gravitaris {

struct cpShapeDeleter {
    void operator()(cpShape* shape) const
    {
        if (shape != nullptr) {
            cpSpace* space = cpShapeGetSpace(shape);
            if (space != nullptr)
                cpSpaceRemoveShape(space, shape);

            cpShapeFree(shape);
        }
    }
};

struct cpBodyDeleter {
    void operator()(cpBody* body) const
    {
        if (body != nullptr) {
            cpSpace* space = cpBodyGetSpace(body);
            if (space != nullptr)
                cpSpaceRemoveBody(space, body);

            cpBodyFree(body);
        }
    }
};

struct cpSpaceDeleter {
    void operator()(cpSpace* space) const
    {
        if (space != nullptr)
            cpSpaceFree(space);
    }
};

typedef std::unique_ptr<cpShape, cpShapeDeleter> cpShapeUniquePtr;
typedef std::unique_ptr<cpBody, cpBodyDeleter> cpBodyUniquePtr;
typedef std::unique_ptr<cpSpace, cpSpaceDeleter> cpSpaceUniquePtr;


} // namespace Gravitaris
