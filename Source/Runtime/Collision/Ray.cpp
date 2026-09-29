#include "EnginePCH.h"
#include "Ray.h"
#include "Rendering/StaticMeshData.h"
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

namespace
{
    // 방향 성분이 0이면 부호를 유지한 아주 작은 값으로 바꾼 뒤 역수를 구한다.
    // (직교 뷰처럼 축과 나란한 레이에서 무한대·NaN이 생기는 것을 막는다)
    float SafeReciprocal(float Value)
    {
        constexpr float MinMagnitude = 1e-20f;
        if (fabsf(Value) < MinMagnitude)
        {
            Value = (Value < 0.0f) ? -MinMagnitude : MinMagnitude;
        }
        return 1.0f / Value;
    }
}

FTraceContext MakeTraceContext(const FRay& WorldRay, FBillboardTraceFn ResolveBillboard, const void* ViewContext)
{
    FTraceContext Context;
    Context.Ray = WorldRay;
    Context.InvDir = FVector(
        SafeReciprocal(WorldRay.Direction.X),
        SafeReciprocal(WorldRay.Direction.Y),
        SafeReciprocal(WorldRay.Direction.Z));
    Context.ResolveBillboard = ResolveBillboard;
    Context.ViewContext = ViewContext;
    return Context;
}

// 기존 RayIntersectsAABB와 같은 slab 판정이지만, 역수를 컨텍스트에서 받아 나눗셈이 없다.
bool RayIntersectsAABB(const FTraceContext& Context, const FVector& BoxMin, const FVector& BoxMax, float& OutTEnter)
{
    const FVector& O = Context.Ray.Origin;
    const FVector& I = Context.InvDir;

    const float tX1 = (BoxMin.X - O.X) * I.X;
    const float tX2 = (BoxMax.X - O.X) * I.X;
    const float tY1 = (BoxMin.Y - O.Y) * I.Y;
    const float tY2 = (BoxMax.Y - O.Y) * I.Y;
    const float tZ1 = (BoxMin.Z - O.Z) * I.Z;
    const float tZ2 = (BoxMax.Z - O.Z) * I.Z;

    const float tEnter = fmaxf(fmaxf(fminf(tX1, tX2), fminf(tY1, tY2)), fminf(tZ1, tZ2));   // 진입점
    const float tExit  = fminf(fminf(fmaxf(tX1, tX2), fmaxf(tY1, tY2)), fmaxf(tZ1, tZ2));   // 이탈점

    if (tEnter > tExit || tExit < 0.0f)
    {   // 빗나감, 또는 박스가 레이 뒤에 있음
        return false;
    }

    OutTEnter = fmaxf(tEnter, 0.0f);   // 레이 시작점이 박스 안이면 0
    return true;
}

bool RayIntersectsTriangle(const FRay& Ray, const FVector& v1, const FVector& v2, const FVector& v3, float& OutT)
{
    constexpr float epsilon = 1e-5f;
    // 평면 정의
    FVector edge1 = v2 - v1;
    FVector edge2 = v3 - v1;

    FVector RayVector = Ray.Direction;

    const FVector rayCrossVec = FVector::Cross(RayVector, edge2);
    float det = FVector::Dot(rayCrossVec, edge1);
    if (det < epsilon)
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
    //if (!RayIntersectsAABB(LocalRay, Box.Min, Box.Max, BoxT))
    //{
    //    return false;
    //}

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
