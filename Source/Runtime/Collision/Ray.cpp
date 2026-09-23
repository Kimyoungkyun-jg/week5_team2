#include "EnginePCH.h"
#include "Ray.h"
#include "Math/EngineMath.h"


FRay ToLocalRay(const FRay& WorldRay, const FMatrix& WorldMatrix)
{
    // ray를 로컬공간으로
    FMatrix invWorld = WorldMatrix.Inverse();
    FRay LocalRay{};
    LocalRay.Origin = invWorld.TransformPosition(WorldRay.Origin);
    LocalRay.Direction = invWorld.TransformVector(WorldRay.Direction);

    return LocalRay;
}

bool RayIntersectsAABB(const FRay& Ray, const FVector& BoxMin, const FVector& BoxMax, float& OutT)
{
    float invRayDir = 1.0f / Ray.Direction.X;
    float tX1 = (BoxMin.X - Ray.Origin.X) * invRayDir;
    float tX2 = (BoxMax.X - Ray.Origin.X) * invRayDir;
    float tMinX = fmin(tX1, tX2);
    float tMaxX = fmax(tX1, tX2);

    invRayDir = 1.0f / Ray.Direction.Y;
    float tY1 = (BoxMin.Y - Ray.Origin.Y) * invRayDir;
    float tY2 = (BoxMax.Y - Ray.Origin.Y) * invRayDir;
    float tMinY = fmin(tY1, tY2);
    float tMaxY = fmax(tY1, tY2);

    invRayDir = 1.0f / Ray.Direction.Z;
    float tZ1 = (BoxMin.Z - Ray.Origin.Z) * invRayDir;
    float tZ2 = (BoxMax.Z - Ray.Origin.Z) * invRayDir;
    float tMinZ = fmin(tZ1, tZ2);
    float tMaxZ = fmax(tZ1, tZ2);   
    
    float tEnter = fmax(fmax(tMinX, tMinY), tMinZ);   // min 중에 가장 큰 값 (진입점)
    float tExit = fmin(fmin(tMaxX, tMaxY), tMaxZ);   // max 중에 가장 작은 값 (이탈점)

    if (tEnter > tExit)
    {   // 충돌 안함
        return false;
    }

    if (tExit < 0.0f)
    {   // 박스가 Ray 뒤에 있을 경우
        return false;
    }

    // 광선이 내부라면 tEnter는 음수. 
    OutT = fmax(0.0f, tEnter);
    return true;
}

bool RayIntersectsTriangle(const FRay& Ray, const FVector& v1, const FVector& v2, const FVector& v3, float& OutT)
{
    constexpr float epsilon = 1e-5f;
    // 평면 정의
    FVector edge1 = v2 - v1;
    FVector edge2 = v3 - v1;

    const FVector normal = FVector::Cross(edge1, edge2);
    FVector RayVector = Ray.Direction;
    if (normal.Dot(RayVector) > 0.0f) // 내적의 결과가 양수면 뒷면임
    {
        return false;
    }

    const FVector rayCrossVec = FVector::Cross(RayVector, edge2);
    float det = FVector::Dot(rayCrossVec, edge1);
    if (fabs(det) < epsilon)
    {   // 내적의 결과가 0에 가까우면 180도. 평행한 관계
        return false;
    }


    float invDet = 1.0f / det;
    // 수식: Ray.Origin - v1 = u * edge1 + v * edge2 - t * Ray.Direction
    // 1. u 구하기
    FVector s = Ray.Origin - v1;
    float u = invDet * FVector::Dot(s, rayCrossVec);

    if (-epsilon > u || epsilon < u - 1)
    {
        return false;
    }

    FVector sCrossE1 = FVector::Cross(s, edge1);
    float v = invDet * FVector::Dot(RayVector, sCrossE1);
        
    if (-epsilon > v || epsilon < u + v - 1)
    {
        return false;
    }

    float t = invDet * FVector::Dot(edge2, sCrossE1);

    if (t > epsilon)
    {
        OutT = t;
        return true;
    }

    return false;
}

// Mesh AABB를 통과한 Ray에 삼각형 교차를 적용해 가장 가까운 거리만 반환한다.
bool RayIntersectsMesh(const FRay& LocalRay, const FStaticMeshData& Mesh, float& OutT)
{
    FBox Box = Mesh.AABB;
    float BoxT{};
    if (!RayIntersectsAABB(LocalRay, Box.Min, Box.Max, BoxT))
    {
        return false;
    }

    bool bHit = false;
    float NearestT = FLT_MAX;
    
    for (uint32 i = 0; i + 2 < Mesh.Indices.Num(); i += 3)
    {
        FVector vertices[3]{};	// 3 vertex
        for (uint32 j = 0; j < 3; ++j)
        {
            uint32 index = Mesh.Indices[i + j];

            vertices[j].X = Mesh.Vertices[index].Position.X;
            vertices[j].Y = Mesh.Vertices[index].Position.Y;
            vertices[j].Z = Mesh.Vertices[index].Position.Z;
        }

        float T = FLT_MAX;
        if (RayIntersectsTriangle(LocalRay, vertices[0], vertices[1], vertices[2], T) && T < NearestT)
        {
            NearestT = T;
            bHit = true;
        }
    }

    if (bHit) OutT = NearestT;
    return bHit;
}

FVector2 WorldToScreen(const FVector& WorldPos, const FMatrix& ViewProj, int ScreenW, int ScreenH)
{
    FVector4 clip = FVector4(WorldPos.X, WorldPos.Y, WorldPos.Z, 1.0f) * ViewProj;

    if (clip.W < 0.0001f)
        return FVector2(-FLT_MAX, -FLT_MAX);

    float ndcX = clip.X / clip.W;
    float ndcY = clip.Y / clip.W;

    FVector2 result;
    result.X = (ndcX * 0.5f + 0.5f) * ScreenW;
    result.Y = (1.0f - (ndcY * 0.5f + 0.5f)) * ScreenH;   // Y 뒤집기
    return result;
}

float DistanceToSegment(const FVector2& P, const FVector2& A, const FVector2& B)
{
    FVector2 seg = B - A;
    float segLenSq = seg.X * seg.X + seg.Y * seg.Y;

    if (segLenSq < 1e-6f)
    {
        FVector2 d = P - A;
        return sqrtf(d.X * d.X + d.Y * d.Y);
    }

    FVector2 toP = P - A;
    float t = (toP.X * seg.X + toP.Y * seg.Y) / segLenSq;

    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);

    FVector2 closest = A + seg * t;
    FVector2 diff = P - closest;
    return sqrtf(diff.X * diff.X + diff.Y * diff.Y);
}

bool RayIntersectsPlane(const FRay& Ray, const FVector& PlanePoint, const FVector& PlaneNormal, float& OutT)
{
    float denom = Ray.Direction.Dot(PlaneNormal);

    if (fabsf(denom) < 1e-6f)
        return false;

    OutT = (PlanePoint - Ray.Origin).Dot(PlaneNormal) / denom;

    return OutT >= 0.0f;
}
