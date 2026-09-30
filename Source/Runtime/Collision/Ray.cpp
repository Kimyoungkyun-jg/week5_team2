#include "EnginePCH.h"
#include "Ray.h"
#include "Rendering/StaticMeshData.h"
#include "Math/EngineMath.h"

#include <algorithm>

namespace
{
constexpr uint32 PickingBVHLeafTriangles = 8;

FVector MinVector(const FVector& A, const FVector& B)
{
	return {std::min(A.X, B.X), std::min(A.Y, B.Y), std::min(A.Z, B.Z)};
}

FVector MaxVector(const FVector& A, const FVector& B)
{
	return {std::max(A.X, B.X), std::max(A.Y, B.Y), std::max(A.Z, B.Z)};
}

FBox GetTriangleBounds(const FStaticMeshData& Mesh, const uint32 TriangleIndex)
{
	const uint32 FirstIndex = TriangleIndex * 3;
	const FVector& A = Mesh.Vertices[Mesh.Indices[FirstIndex]].Position;
	const FVector& B = Mesh.Vertices[Mesh.Indices[FirstIndex + 1]].Position;
	const FVector& C = Mesh.Vertices[Mesh.Indices[FirstIndex + 2]].Position;
	return {MinVector(A, MinVector(B, C)), MaxVector(A, MaxVector(B, C))};
}

FVector GetTriangleCentroid(const FStaticMeshData& Mesh, const uint32 TriangleIndex)
{
	const uint32 FirstIndex = TriangleIndex * 3;
	return (Mesh.Vertices[Mesh.Indices[FirstIndex]].Position + Mesh.Vertices[Mesh.Indices[FirstIndex + 1]].Position + Mesh.Vertices[Mesh.Indices[FirstIndex + 2]].Position) / 3.0f;
}

float AxisValue(const FVector& Value, const int32 Axis)
{
	if (Axis == 0)
		return Value.X;
	if (Axis == 1)
		return Value.Y;
	return Value.Z;
}

uint32 BuildPickingBVHNode(const FStaticMeshData& Mesh, const uint32 First, const uint32 Count)
{
	const uint32 NodeIndex = Mesh.PickingBVHNodes.Add(FMeshPickingBVHNode{});
	FBox Bounds = GetTriangleBounds(Mesh, Mesh.PickingTriangleIndices[First]);
	FVector CentroidMin = GetTriangleCentroid(Mesh, Mesh.PickingTriangleIndices[First]);
	FVector CentroidMax = CentroidMin;

	for (uint32 Offset = 1; Offset < Count; ++Offset)
	{
		const uint32 TriangleIndex = Mesh.PickingTriangleIndices[First + Offset];
		const FBox TriangleBounds = GetTriangleBounds(Mesh, TriangleIndex);
		Bounds.Min = MinVector(Bounds.Min, TriangleBounds.Min);
		Bounds.Max = MaxVector(Bounds.Max, TriangleBounds.Max);
		const FVector Centroid = GetTriangleCentroid(Mesh, TriangleIndex);
		CentroidMin = MinVector(CentroidMin, Centroid);
		CentroidMax = MaxVector(CentroidMax, Centroid);
	}

	if (Count <= PickingBVHLeafTriangles)
	{
		FMeshPickingBVHNode& Node = Mesh.PickingBVHNodes[NodeIndex];
		Node.Bounds = Bounds;
		Node.First = First;
		Node.Count = Count;
		Node.bLeaf = true;
		return NodeIndex;
	}

	const FVector CentroidExtent = CentroidMax - CentroidMin;
	int32 SplitAxis = 0;
	if (CentroidExtent.Y > CentroidExtent.X)
		SplitAxis = 1;
	if (AxisValue(CentroidExtent, 2) > AxisValue(CentroidExtent, SplitAxis))
		SplitAxis = 2;

	const uint32 LeftCount = Count / 2;
	auto Begin = Mesh.PickingTriangleIndices.begin() + First;
	auto Middle = Begin + LeftCount;
	auto End = Begin + Count;
	std::nth_element(Begin,
		Middle,
		End,
		[&](const uint32 A, const uint32 B)
		{
			return AxisValue(GetTriangleCentroid(Mesh, A), SplitAxis) < AxisValue(GetTriangleCentroid(Mesh, B), SplitAxis);
		});

	const uint32 Left = BuildPickingBVHNode(Mesh, First, LeftCount);
	const uint32 Right = BuildPickingBVHNode(Mesh, First + LeftCount, Count - LeftCount);
	FMeshPickingBVHNode& Node = Mesh.PickingBVHNodes[NodeIndex];
	Node.Bounds = Bounds;
	Node.Left = Left;
	Node.Right = Right;
	return NodeIndex;
}

void EnsurePickingBVH(const FStaticMeshData& Mesh)
{
	if (Mesh.bPickingBVHBuilt)
		return;

	Mesh.PickingTriangleIndices.Reset();
	Mesh.PickingBVHNodes.Reset();
	const uint32 TriangleCount = static_cast<uint32>(Mesh.Indices.Num() / 3);
	if (TriangleCount > PickingBVHLeafTriangles)
	{
		Mesh.PickingTriangleIndices.Reserve(TriangleCount);
		for (uint32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
			Mesh.PickingTriangleIndices.Add(TriangleIndex);
		Mesh.PickingBVHNodes.Reserve(TriangleCount * 2);
		BuildPickingBVHNode(Mesh, 0, TriangleCount);
	}
	Mesh.bPickingBVHBuilt = true;
}

void TraceTriangle(const FRay& Ray, const FStaticMeshData& Mesh, const uint32 TriangleIndex, float& InOutNearestT, bool& bInOutHit)
{
	const uint32 FirstIndex = TriangleIndex * 3;
	const FVector& A = Mesh.Vertices[Mesh.Indices[FirstIndex]].Position;
	const FVector& B = Mesh.Vertices[Mesh.Indices[FirstIndex + 1]].Position;
	const FVector& C = Mesh.Vertices[Mesh.Indices[FirstIndex + 2]].Position;
	float T = FLT_MAX;
	if (RayIntersectsTriangle(Ray, A, B, C, T) && T < InOutNearestT)
	{
		InOutNearestT = T;
		bInOutHit = true;
	}
}

// NodeDistance는 부모(또는 루트 검사)가 이미 구한 이 노드의 박스 진입 거리다. 박스를 다시 검사하지 않는다.
void TracePickingBVHNode(const FTraceContext& Context, const FStaticMeshData& Mesh, const uint32 NodeIndex, const float NodeDistance, float& InOutNearestT, bool& bInOutHit)
{
	if (NodeDistance >= InOutNearestT)
		return;

	const FMeshPickingBVHNode& Node = Mesh.PickingBVHNodes[NodeIndex];
	if (Node.bLeaf)
	{
		for (uint32 Offset = 0; Offset < Node.Count; ++Offset)
			TraceTriangle(Context.Ray, Mesh, Mesh.PickingTriangleIndices[Node.First + Offset], InOutNearestT, bInOutHit);
		return;
	}

	const FMeshPickingBVHNode& LeftNode = Mesh.PickingBVHNodes[Node.Left];
	const FMeshPickingBVHNode& RightNode = Mesh.PickingBVHNodes[Node.Right];
	float LeftDistance = 0.0f;
	float RightDistance = 0.0f;
	const bool bHitLeft = RayIntersectsAABB(Context, LeftNode.Bounds.Min, LeftNode.Bounds.Max, LeftDistance);
	const bool bHitRight = RayIntersectsAABB(Context, RightNode.Bounds.Min, RightNode.Bounds.Max, RightDistance);

	if (bHitLeft && bHitRight)
	{
		const uint32 NearNode = LeftDistance <= RightDistance ? Node.Left : Node.Right;
		const uint32 FarNode = LeftDistance <= RightDistance ? Node.Right : Node.Left;
		const float NearDistance = LeftDistance <= RightDistance ? LeftDistance : RightDistance;
		const float FarDistance = LeftDistance <= RightDistance ? RightDistance : LeftDistance;
		TracePickingBVHNode(Context, Mesh, NearNode, NearDistance, InOutNearestT, bInOutHit);
		TracePickingBVHNode(Context, Mesh, FarNode, FarDistance, InOutNearestT, bInOutHit);
	}
	else if (bHitLeft)
	{
		TracePickingBVHNode(Context, Mesh, Node.Left, LeftDistance, InOutNearestT, bInOutHit);
	}
	else if (bHitRight)
	{
		TracePickingBVHNode(Context, Mesh, Node.Right, RightDistance, InOutNearestT, bInOutHit);
	}
}
} // namespace

void PrepareMeshPickingBVH(const FStaticMeshData& Mesh)
{
	EnsurePickingBVH(Mesh);
}

FRay ToLocalRay(const FRay& WorldRay, const FMatrix& WorldMatrix)
{
	// ray를 로컬공간으로
	FMatrix invWorld = WorldMatrix.Inverse();
	FRay LocalRay{};
	LocalRay.Origin = invWorld.TransformPosition(WorldRay.Origin);
	LocalRay.Direction = invWorld.TransformVector(WorldRay.Direction);

	return LocalRay;
}

bool ToLocalRayAffine(const FRay& WorldRay, const FMatrix& WorldMatrix, FRay& OutLocalRay)
{
	// 행벡터 규약(v * M): 0~2행은 축, 3행은 이동. 역행렬의 열 = 두 축의 외적 / det
	const FVector R0(WorldMatrix.M[0][0], WorldMatrix.M[0][1], WorldMatrix.M[0][2]);
	const FVector R1(WorldMatrix.M[1][0], WorldMatrix.M[1][1], WorldMatrix.M[1][2]);
	const FVector R2(WorldMatrix.M[2][0], WorldMatrix.M[2][1], WorldMatrix.M[2][2]);
	const FVector C0 = FVector::Cross(R1, R2);
	const FVector C1 = FVector::Cross(R2, R0);
	const FVector C2 = FVector::Cross(R0, R1);
	const float Det = FVector::Dot(R0, C0);
	if (fabsf(Det) < 1e-12f)
		return false;

	const float InvDet = 1.0f / Det;
	const FVector P(WorldRay.Origin.X - WorldMatrix.M[3][0], WorldRay.Origin.Y - WorldMatrix.M[3][1], WorldRay.Origin.Z - WorldMatrix.M[3][2]);
	OutLocalRay.Origin = FVector(FVector::Dot(P, C0), FVector::Dot(P, C1), FVector::Dot(P, C2)) * InvDet;
	OutLocalRay.Direction = FVector(FVector::Dot(WorldRay.Direction, C0), FVector::Dot(WorldRay.Direction, C1), FVector::Dot(WorldRay.Direction, C2)) * InvDet;
	return true;
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

	float tEnter = fmax(fmax(tMinX, tMinY), tMinZ); // min 중에 가장 큰 값 (진입점)
	float tExit = fmin(fmin(tMaxX, tMaxY), tMaxZ);  // max 중에 가장 작은 값 (이탈점)

	if (tEnter > tExit)
	{ // 충돌 안함
		return false;
	}

	if (tExit < 0.0f)
	{ // 박스가 Ray 뒤에 있을 경우
		return false;
	}

	// 광선이 내부라면 tEnter는 음수.
	OutT = fmax(0.0f, tEnter);
	return true;
}

bool RayIntersectsBoundingSphere(const FRay& Ray, const FVector& SphereCenter, const float SphereRadius, float& OutT)
{
    if (SphereRadius < 0.0f) return false;

    // 임시 FVector 생성/정규화 없이 성분으로 계산한다.
    const float OX = Ray.Origin.X - SphereCenter.X;
    const float OY = Ray.Origin.Y - SphereCenter.Y;
    const float OZ = Ray.Origin.Z - SphereCenter.Z;
    const float RadiusSquared = SphereRadius * SphereRadius;
    const float C = OX * OX + OY * OY + OZ * OZ - RadiusSquared;
    if (C <= 0.0f)
    {
        OutT = 0.0f;
        return true;
    }

    const float DX = Ray.Direction.X;
    const float DY = Ray.Direction.Y;
    const float DZ = Ray.Direction.Z;
    const float B = OX * DX + OY * DY + OZ * DZ;
    // 구 밖에서 멀어지는 Ray(방향 0 포함)는 제곱근/나눗셈 전에 탈락한다.
    if (B >= 0.0f) return false;

    const float A = DX * DX + DY * DY + DZ * DZ;
    if (A <= 0.0f) return false;

    // B*B - A*C와 동치. 먼 작은 구에서 큰 두 수를 빼는 정밀도 손실을 줄인다.
    const float CrossX = OY * DZ - OZ * DY;
    const float CrossY = OZ * DX - OX * DZ;
    const float CrossZ = OX * DY - OY * DX;
    const float Discriminant = A * RadiusSquared -
        (CrossX * CrossX + CrossY * CrossY + CrossZ * CrossZ);
    if (Discriminant < 0.0f) return false;

    // (-B - sqrt(D)) / A를 유리화: 표면 가까이에서 뺄셈 오차를 줄인다.
    // 외부의 실제 교차에서만 sqrt 1회, 나눗셈 1회를 수행한다. 로컬 Ray의 t도 보존된다.
    OutT = C / (-B + sqrtf(Discriminant));
    return true;
}

bool RayIntersectsBoundingSphere(const FTraceContext& Context, const FVector& SphereCenter, const float SphereRadius, float& OutTEnter)
{
    return RayIntersectsBoundingSphere(Context.Ray, SphereCenter, SphereRadius, OutTEnter);
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
} // namespace

FTraceContext MakeTraceContext(const FRay& WorldRay, FBillboardTraceFn ResolveBillboard, const void* ViewContext)
{
	FTraceContext Context;
	Context.Ray = WorldRay;
	Context.InvDir = FVector(SafeReciprocal(WorldRay.Direction.X), SafeReciprocal(WorldRay.Direction.Y), SafeReciprocal(WorldRay.Direction.Z));
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

	const float tEnter = fmaxf(fmaxf(fminf(tX1, tX2), fminf(tY1, tY2)), fminf(tZ1, tZ2)); // 진입점
	const float tExit = fminf(fminf(fmaxf(tX1, tX2), fmaxf(tY1, tY2)), fmaxf(tZ1, tZ2));  // 이탈점

	if (tEnter > tExit || tExit < 0.0f)
	{ // 빗나감, 또는 박스가 레이 뒤에 있음
		return false;
	}

	OutTEnter = fmaxf(tEnter, 0.0f); // 레이 시작점이 박스 안이면 0
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
	{ // 내적의 결과가 0에 가까우면 180도. 평행한 관계
		return false;
	}

	// det > 0이므로 u, v, t의 비교를 det 곱으로 옮겨 나눗셈을 통과한 삼각형에서만 한다.
	// 수식: Ray.Origin - v1 = u * edge1 + v * edge2 - t * Ray.Direction
	const float epsDet = epsilon * det;
	const float upperDet = det * (1.0f + epsilon);

	// 1. u 구하기 (u = uNum / det)
	FVector s = Ray.Origin - v1;
	const float uNum = FVector::Dot(s, rayCrossVec);

	if (uNum < -epsDet || uNum > upperDet)
	{
		return false;
	}

	FVector sCrossE1 = FVector::Cross(s, edge1);
	const float vNum = FVector::Dot(RayVector, sCrossE1);

	if (vNum < -epsDet || uNum + vNum > upperDet)
	{
		return false;
	}

	const float tNum = FVector::Dot(edge2, sCrossE1);

	if (tNum > epsDet)
	{
		OutT = tNum / det;
		return true;
	}

	return false;
}

// Mesh AABB를 통과한 Ray에 삼각형 교차를 적용해 가장 가까운 거리만 반환한다.
bool RayIntersectsMesh(const FRay& LocalRay, const FStaticMeshData& Mesh, float& OutT, const float MaxT)
{
	PrepareMeshPickingBVH(Mesh);
	bool bHit = false;
	float NearestT = MaxT;

	if (!Mesh.PickingBVHNodes.IsEmpty())
	{
		// 로컬 레이의 역수는 메시당 한 번만 구해 모든 노드가 공유한다.
		const FTraceContext Context = MakeTraceContext(LocalRay, nullptr, nullptr);
		const FMeshPickingBVHNode& Root = Mesh.PickingBVHNodes[0];
		float RootDistance = 0.0f;
		if (RayIntersectsAABB(Context, Root.Bounds.Min, Root.Bounds.Max, RootDistance))
			TracePickingBVHNode(Context, Mesh, 0, RootDistance, NearestT, bHit);
	}
	else
	{
		const uint32 TriangleCount = static_cast<uint32>(Mesh.Indices.Num() / 3);
		for (uint32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
			TraceTriangle(LocalRay, Mesh, TriangleIndex, NearestT, bHit);
	}

	if (bHit)
		OutT = NearestT;
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
	result.Y = (1.0f - (ndcY * 0.5f + 0.5f)) * ScreenH; // Y 뒤집기
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
