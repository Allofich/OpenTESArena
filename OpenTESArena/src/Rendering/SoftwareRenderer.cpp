#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

#include "ArenaRenderUtils.h"
#include "RenderCamera.h"
#include "RenderDrawCall.h"
#include "RendererUtils.h"
#include "RenderFrameSettings.h"
#include "RenderInitSettings.h"
#include "RenderTransform.h"
#include "SoftwareRenderer.h"
#include "../Assets/TextureBuilder.h"
#include "../Math/Constants.h"
#include "../Math/MathUtils.h"
#include "../Math/Random.h"
#include "../Utilities/Color.h"
#include "../Utilities/Palette.h"
#include "../World/ChunkUtils.h"

#include "components/debug/Debug.h"

namespace
{
	constexpr int TYPICAL_STEP_COUNT = 4; // Elements processed per unrolled loop, possibly also for SIMD lanes.

	// Optimized math functions.
	void Double4_Zero4(double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < 4; i++)
		{
			outXs[i] = 0.0;
			outYs[i] = 0.0;
			outZs[i] = 0.0;
			outWs[i] = 0.0;
		}
	}

	void Double4_Load4(const Double4 &v0, const Double4 &v1, const Double4 &v2, const Double4 &v3,
		double *outXs, double *outYs, double *outZs, double *outWs)
	{
		const Double4 *vs[] = { &v0, &v1, &v2, &v3 };

		for (int i = 0; i < 4; i++)
		{
			const Double4 &v = *vs[i];
			outXs[i] = v.x;
			outYs[i] = v.y;
			outZs[i] = v.z;
			outWs[i] = v.w;
		}
	}

	template<int N>
	void Double4_AddN(const double *x0s, const double *y0s, const double *z0s, const double *w0s, const double *x1s, const double *y1s,
		const double *z1s, const double *w1s, double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = x0s[i] + x1s[i];
			outYs[i] = y0s[i] + y1s[i];
			outZs[i] = z0s[i] + z1s[i];
			outWs[i] = w0s[i] + w1s[i];
		}
	}

	void Double4_Negate4(const double *xs, const double *ys, const double *zs, const double *ws, double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < 4; i++)
		{
			outXs[i] = -xs[i];
			outYs[i] = -ys[i];
			outZs[i] = -zs[i];
			outWs[i] = -ws[i];
		}
	}

	template<int N>
	void Double4_SubtractN(const double *x0s, const double *y0s, const double *z0s, const double *w0s, const double *x1s, const double *y1s,
		const double *z1s, const double *w1s, double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = x0s[i] - x1s[i];
			outYs[i] = y0s[i] - y1s[i];
			outZs[i] = z0s[i] - z1s[i];
			outWs[i] = w0s[i] - w1s[i];
		}
	}

	void Double4_Multiply4(const double *x0s, const double *y0s, const double *z0s, const double *w0s, const double *x1s, const double *y1s,
		const double *z1s, const double *w1s, double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < 4; i++)
		{
			outXs[i] = x0s[i] * x1s[i];
			outYs[i] = y0s[i] * y1s[i];
			outZs[i] = z0s[i] * z1s[i];
			outWs[i] = w0s[i] * w1s[i];
		}
	}

	void Double4_Divide4(const double *x0s, const double *y0s, const double *z0s, const double *w0s, const double *x1s, const double *y1s,
		const double *z1s, const double *w1s, double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < 4; i++)
		{
			outXs[i] = x0s[i] / x1s[i];
			outYs[i] = y0s[i] / y1s[i];
			outZs[i] = z0s[i] / z1s[i];
			outWs[i] = w0s[i] / w1s[i];
		}
	}

	void Matrix4_Zero4(double *outMxxs, double *outMxys, double *outMxzs, double *outMxws,
		double *outMyxs, double *outMyys, double *outMyzs, double *outMyws,
		double *outMzxs, double *outMzys, double *outMzzs, double *outMzws,
		double *outMwxs, double *outMwys, double *outMwzs, double *outMwws)
	{
		for (int i = 0; i < 4; i++)
		{
			outMxxs[i] = 0.0;
			outMxys[i] = 0.0;
			outMxzs[i] = 0.0;
			outMxws[i] = 0.0;
			outMyxs[i] = 0.0;
			outMyys[i] = 0.0;
			outMyzs[i] = 0.0;
			outMyws[i] = 0.0;
			outMzxs[i] = 0.0;
			outMzys[i] = 0.0;
			outMzzs[i] = 0.0;
			outMzws[i] = 0.0;
			outMwxs[i] = 0.0;
			outMwys[i] = 0.0;
			outMwzs[i] = 0.0;
			outMwws[i] = 0.0;
		}
	}

	void Matrix4_Load4(const Matrix4d &m0, const Matrix4d &m1, const Matrix4d &m2, const Matrix4d &m3,
		double *outMxxs, double *outMxys, double *outMxzs, double *outMxws,
		double *outMyxs, double *outMyys, double *outMyzs, double *outMyws,
		double *outMzxs, double *outMzys, double *outMzzs, double *outMzws,
		double *outMwxs, double *outMwys, double *outMwzs, double *outMwws)
	{
		const Matrix4d *ms[] = { &m0, &m1, &m2, &m3 };

		for (int i = 0; i < 4; i++)
		{
			const Matrix4d &m = *ms[i];
			outMxxs[i] = m.x.x;
			outMxys[i] = m.x.y;
			outMxzs[i] = m.x.z;
			outMxws[i] = m.x.w;
			outMyxs[i] = m.y.x;
			outMyys[i] = m.y.y;
			outMyzs[i] = m.y.z;
			outMyws[i] = m.y.w;
			outMzxs[i] = m.z.x;
			outMzys[i] = m.z.y;
			outMzzs[i] = m.z.z;
			outMzws[i] = m.z.w;
			outMwxs[i] = m.w.x;
			outMwys[i] = m.w.y;
			outMwzs[i] = m.w.z;
			outMwws[i] = m.w.w;
		}
	}

	template<int N>
	void Matrix4_MultiplyVectorN(const double *mxxs, const double *mxys, const double *mxzs, const double *mxws,
		const double *myxs, const double *myys, const double *myzs, const double *myws,
		const double *mzxs, const double *mzys, const double *mzzs, const double *mzws,
		const double *mwxs, const double *mwys, const double *mwzs, const double *mwws,
		const double *xs, const double *ys, const double *zs, const double *ws,
		double *outXs, double *outYs, double *outZs, double *outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] += (mxxs[i] * xs[i]) + (myxs[i] * ys[i]) + (mzxs[i] * zs[i]) + (mwxs[i] * ws[i]);
			outYs[i] += (mxys[i] * xs[i]) + (myys[i] * ys[i]) + (mzys[i] * zs[i]) + (mwys[i] * ws[i]);
			outZs[i] += (mxzs[i] * xs[i]) + (myzs[i] * ys[i]) + (mzzs[i] * zs[i]) + (mwzs[i] * ws[i]);
			outWs[i] += (mxws[i] * xs[i]) + (myws[i] * ys[i]) + (mzws[i] * zs[i]) + (mwws[i] * ws[i]);
		}
	}

	template<int N>
	void Matrix4_MultiplyMatrixN(const double *m0xxs, const double *m0xys, const double *m0xzs, const double *m0xws,
		const double *m0yxs, const double *m0yys, const double *m0yzs, const double *m0yws,
		const double *m0zxs, const double *m0zys, const double *m0zzs, const double *m0zws,
		const double *m0wxs, const double *m0wys, const double *m0wzs, const double *m0wws,
		const double *m1xxs, const double *m1xys, const double *m1xzs, const double *m1xws,
		const double *m1yxs, const double *m1yys, const double *m1yzs, const double *m1yws,
		const double *m1zxs, const double *m1zys, const double *m1zzs, const double *m1zws,
		const double *m1wxs, const double *m1wys, const double *m1wzs, const double *m1wws,
		double *outMxxs, double *outMxys, double *outMxzs, double *outMxws,
		double *outMyxs, double *outMyys, double *outMyzs, double *outMyws,
		double *outMzxs, double *outMzys, double *outMzzs, double *outMzws,
		double *outMwxs, double *outMwys, double *outMwzs, double *outMwws)
	{
		for (int i = 0; i < N; i++)
		{
			outMxxs[i] = (m0xxs[i] * m1xxs[i]) + (m0yxs[i] * m1xys[i]) + (m0zxs[i] * m1xzs[i]) + (m0wxs[i] * m1xws[i]);
			outMxys[i] = (m0xys[i] * m1xxs[i]) + (m0yys[i] * m1xys[i]) + (m0zys[i] * m1xzs[i]) + (m0wys[i] * m1xws[i]);
			outMxzs[i] = (m0xzs[i] * m1xxs[i]) + (m0yzs[i] * m1xys[i]) + (m0zzs[i] * m1xzs[i]) + (m0wzs[i] * m1xws[i]);
			outMxws[i] = (m0xws[i] * m1xxs[i]) + (m0yws[i] * m1xys[i]) + (m0zws[i] * m1xzs[i]) + (m0wws[i] * m1xws[i]);
			outMyxs[i] = (m0xxs[i] * m1yxs[i]) + (m0yxs[i] * m1yys[i]) + (m0zxs[i] * m1yzs[i]) + (m0wxs[i] * m1yws[i]);
			outMyys[i] = (m0xys[i] * m1yxs[i]) + (m0yys[i] * m1yys[i]) + (m0zys[i] * m1yzs[i]) + (m0wys[i] * m1yws[i]);
			outMyzs[i] = (m0xzs[i] * m1yxs[i]) + (m0yzs[i] * m1yys[i]) + (m0zzs[i] * m1yzs[i]) + (m0wzs[i] * m1yws[i]);
			outMyws[i] = (m0xws[i] * m1yxs[i]) + (m0yws[i] * m1yys[i]) + (m0zws[i] * m1yzs[i]) + (m0wws[i] * m1yws[i]);
			outMzxs[i] = (m0xxs[i] * m1zxs[i]) + (m0yxs[i] * m1zys[i]) + (m0zxs[i] * m1zzs[i]) + (m0wxs[i] * m1zws[i]);
			outMzys[i] = (m0xys[i] * m1zxs[i]) + (m0yys[i] * m1zys[i]) + (m0zys[i] * m1zzs[i]) + (m0wys[i] * m1zws[i]);
			outMzzs[i] = (m0xzs[i] * m1zxs[i]) + (m0yzs[i] * m1zys[i]) + (m0zzs[i] * m1zzs[i]) + (m0wzs[i] * m1zws[i]);
			outMzws[i] = (m0xws[i] * m1zxs[i]) + (m0yws[i] * m1zys[i]) + (m0zws[i] * m1zzs[i]) + (m0wws[i] * m1zws[i]);
			outMwxs[i] = (m0xxs[i] * m1wxs[i]) + (m0yxs[i] * m1wys[i]) + (m0zxs[i] * m1wzs[i]) + (m0wxs[i] * m1wws[i]);
			outMwys[i] = (m0xys[i] * m1wxs[i]) + (m0yys[i] * m1wys[i]) + (m0zys[i] * m1wzs[i]) + (m0wys[i] * m1wws[i]);
			outMwzs[i] = (m0xzs[i] * m1wxs[i]) + (m0yzs[i] * m1wys[i]) + (m0zzs[i] * m1wzs[i]) + (m0wzs[i] * m1wws[i]);
			outMwws[i] = (m0xws[i] * m1wxs[i]) + (m0yws[i] * m1wys[i]) + (m0zws[i] * m1wzs[i]) + (m0wws[i] * m1wws[i]);
		}
	}

	// Internal camera globals.
	Matrix4d g_viewMatrix;
	Matrix4d g_projMatrix;

	Matrix4d g_viewProjMatrix;
	double g_viewProjMatrixXX[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixXY[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixXZ[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixXW[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixYX[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixYY[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixYZ[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixYW[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixZX[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixZY[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixZZ[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixZW[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixWX[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixWY[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixWZ[TYPICAL_STEP_COUNT];
	double g_viewProjMatrixWW[TYPICAL_STEP_COUNT];

	Matrix4d g_invViewMatrix;
	Matrix4d g_invProjMatrix;

	void PopulateCameraGlobals(const RenderCamera &camera)
	{
		g_viewMatrix = camera.viewMatrix;
		g_projMatrix = camera.projectionMatrix;

		g_viewProjMatrix = camera.projectionMatrix * camera.viewMatrix;
		for (int i = 0; i < TYPICAL_STEP_COUNT; i++)
		{
			g_viewProjMatrixXX[i] = g_viewProjMatrix.x.x;
			g_viewProjMatrixXY[i] = g_viewProjMatrix.x.y;
			g_viewProjMatrixXZ[i] = g_viewProjMatrix.x.z;
			g_viewProjMatrixXW[i] = g_viewProjMatrix.x.w;
			g_viewProjMatrixYX[i] = g_viewProjMatrix.y.x;
			g_viewProjMatrixYY[i] = g_viewProjMatrix.y.y;
			g_viewProjMatrixYZ[i] = g_viewProjMatrix.y.z;
			g_viewProjMatrixYW[i] = g_viewProjMatrix.y.w;
			g_viewProjMatrixZX[i] = g_viewProjMatrix.z.x;
			g_viewProjMatrixZY[i] = g_viewProjMatrix.z.y;
			g_viewProjMatrixZZ[i] = g_viewProjMatrix.z.z;
			g_viewProjMatrixZW[i] = g_viewProjMatrix.z.w;
			g_viewProjMatrixWX[i] = g_viewProjMatrix.w.x;
			g_viewProjMatrixWY[i] = g_viewProjMatrix.w.y;
			g_viewProjMatrixWZ[i] = g_viewProjMatrix.w.z;
			g_viewProjMatrixWW[i] = g_viewProjMatrix.w.w;
		}

		g_invViewMatrix = camera.inverseViewMatrix;
		g_invProjMatrix = camera.inverseProjectionMatrix;
	}

	// Internal mesh processing globals.
	constexpr int MAX_DRAW_CALL_MESH_TRIANGLES = 1024; // The most triangles a draw call mesh can have. Used with vertex shading.
	constexpr int MAX_MESH_PROCESS_CACHES = 8; // The most draw call meshes that can be cached and processed each loop.
	constexpr int MAX_VERTEX_SHADING_CACHE_TRIANGLES = MAX_DRAW_CALL_MESH_TRIANGLES * 2; // The most unshaded triangles that can be cached for the vertex shader loop.
	constexpr int MAX_CLIPPED_MESH_TRIANGLES = 4096; // The most triangles a processed clip space mesh can have when passed to the rasterizer.
	constexpr int MAX_CLIPPED_TRIANGLE_TRIANGLES = 64; // The most triangles a triangle can generate after being clipped by all clip planes.

	// Bulk draw call processing caches sharing a vertex shader to calculate clipped meshes for rasterizing.
	// Struct-of-arrays layout for speed.
	struct MeshProcessCaches
	{
		double translationMatrixXXs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixXYs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixXZs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixXWs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixYXs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixYYs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixYZs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixYWs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixZXs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixZYs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixZZs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixZWs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixWXs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixWYs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixWZs[MAX_MESH_PROCESS_CACHES];
		double translationMatrixWWs[MAX_MESH_PROCESS_CACHES];

		double rotationMatrixXXs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixXYs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixXZs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixXWs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixYXs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixYYs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixYZs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixYWs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixZXs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixZYs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixZZs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixZWs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixWXs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixWYs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixWZs[MAX_MESH_PROCESS_CACHES];
		double rotationMatrixWWs[MAX_MESH_PROCESS_CACHES];

		double scaleMatrixXXs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixXYs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixXZs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixXWs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixYXs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixYYs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixYZs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixYWs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixZXs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixZYs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixZZs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixZWs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixWXs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixWYs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixWZs[MAX_MESH_PROCESS_CACHES];
		double scaleMatrixWWs[MAX_MESH_PROCESS_CACHES];

		double modelViewProjMatrixXXs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixXYs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixXZs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixXWs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixYXs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixYYs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixYZs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixYWs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixZXs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixZYs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixZZs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixZWs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixWXs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixWYs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixWZs[MAX_MESH_PROCESS_CACHES];
		double modelViewProjMatrixWWs[MAX_MESH_PROCESS_CACHES];

		double preScaleTranslationXs[MAX_MESH_PROCESS_CACHES];
		double preScaleTranslationYs[MAX_MESH_PROCESS_CACHES];
		double preScaleTranslationZs[MAX_MESH_PROCESS_CACHES];

		const SoftwareRenderer::VertexBuffer *vertexBuffers[MAX_MESH_PROCESS_CACHES];
		const SoftwareRenderer::AttributeBuffer *texCoordBuffers[MAX_MESH_PROCESS_CACHES];
		const SoftwareRenderer::IndexBuffer *indexBuffers[MAX_MESH_PROCESS_CACHES];
		ObjectTextureID textureID0s[MAX_MESH_PROCESS_CACHES];
		ObjectTextureID textureID1s[MAX_MESH_PROCESS_CACHES];
		TextureSamplingType textureSamplingType0s[MAX_MESH_PROCESS_CACHES];
		TextureSamplingType textureSamplingType1s[MAX_MESH_PROCESS_CACHES];
		RenderLightingType lightingTypes[MAX_MESH_PROCESS_CACHES];
		double meshLightPercents[MAX_MESH_PROCESS_CACHES];
		const SoftwareRenderer::Light *lightPtrArrays[MAX_MESH_PROCESS_CACHES][RenderLightIdList::MAX_LIGHTS];
		int lightCounts[MAX_MESH_PROCESS_CACHES];
		PixelShaderType pixelShaderTypes[MAX_MESH_PROCESS_CACHES];
		double pixelShaderParam0s[MAX_MESH_PROCESS_CACHES];
		bool enableDepthReads[MAX_MESH_PROCESS_CACHES];
		bool enableDepthWrites[MAX_MESH_PROCESS_CACHES];

		// Vertex shader results to be iterated over in the clipping stage.
		Double4 shadedV0Arrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES];
		Double4 shadedV1Arrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES];
		Double4 shadedV2Arrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES];
		Double2 uv0Arrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES];
		Double2 uv1Arrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES];
		Double2 uv2Arrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES];
		int triangleWriteCounts[MAX_MESH_PROCESS_CACHES]; // This should match the draw call triangle count.

		// Triangles generated by clipping the current mesh. These are sent to the rasterizer.
		Double4 clipSpaceMeshV0Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];
		Double4 clipSpaceMeshV1Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];
		Double4 clipSpaceMeshV2Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];
		Double2 clipSpaceMeshUV0Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];
		Double2 clipSpaceMeshUV1Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];
		Double2 clipSpaceMeshUV2Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];

		// Triangles generated by clipping the current triangle against clipping planes.
		Double4 clipSpaceTriangleV0Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES];
		Double4 clipSpaceTriangleV1Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES];
		Double4 clipSpaceTriangleV2Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES];
		Double2 clipSpaceTriangleUV0Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES];
		Double2 clipSpaceTriangleUV1Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES];
		Double2 clipSpaceTriangleUV2Arrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES];

		// Triangles in the current clip space mesh to be rasterized.
		int clipSpaceMeshTriangleCounts[MAX_MESH_PROCESS_CACHES];
	};

	MeshProcessCaches g_meshProcessCaches;

	// Pixel and vertex shading definitions.
	struct PixelShaderPerspectiveCorrection
	{
		double ndcZDepth;
		Double2 texelPercent;
	};

	struct PixelShaderTexture
	{
		const uint8_t *texels;
		int width, height;
		int widthMinusOne, heightMinusOne;
		double widthReal, heightReal;
		TextureSamplingType samplingType;

		void init(const uint8_t *texels, int width, int height, TextureSamplingType samplingType)
		{
			this->texels = texels;
			this->width = width;
			this->height = height;
			this->widthMinusOne = width - 1;
			this->heightMinusOne = height - 1;
			this->widthReal = static_cast<double>(width);
			this->heightReal = static_cast<double>(height);
			this->samplingType = samplingType;
		}
	};

	struct PixelShaderPalette
	{
		const uint32_t *colors;
		int count;
	};

	struct PixelShaderLighting
	{
		const uint8_t *lightTableTexels;
		int lightLevelCount; // # of shades from light to dark.
		double lightLevelCountReal;
		int lastLightLevel;
		int texelsPerLightLevel; // Should be 256 for 8-bit colors.
		int lightLevel; // The selected row of shades between light and dark.
	};

	struct PixelShaderHorizonMirror
	{
		Double2 horizonScreenSpacePoint; // Based on camera forward direction as XZ vector.
		int reflectedPixelIndex;
		bool isReflectedPixelInFrameBuffer;
		uint8_t fallbackSkyColor;
	};

	struct PixelShaderFrameBuffer
	{
		uint8_t *colors;
		double *depth;
		PixelShaderPalette palette;
		double xPercent, yPercent;
		int pixelIndex;
		bool enableDepthWrite;
	};

	template<int stepCount>
	void VertexShader_Basic(int meshIndex, const double *vertexXs, const double *vertexYs, const double *vertexZs, const double *vertexWs,
		double *outVertexXs, double *outVertexYs, double *outVertexZs, double *outVertexWs)
	{
		// Apply model-view-projection matrix.
		Matrix4_MultiplyVectorN<stepCount>(
			g_meshProcessCaches.modelViewProjMatrixXXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXWs + meshIndex,
			g_meshProcessCaches.modelViewProjMatrixYXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYWs + meshIndex,
			g_meshProcessCaches.modelViewProjMatrixZXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZWs + meshIndex,
			g_meshProcessCaches.modelViewProjMatrixWXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWWs + meshIndex,
			vertexXs, vertexYs, vertexZs, vertexWs,
			outVertexXs, outVertexYs, outVertexZs, outVertexWs);
	}

	template<int stepCount>
	void VertexShader_RaisingDoor(int meshIndex, const double *vertexXs, const double *vertexYs, const double *vertexZs, const double *vertexWs,
		double *outVertexXs, double *outVertexYs, double *outVertexZs, double *outVertexWs)
	{
		const double preScaleTranslationWs[stepCount] = { 0.0 };

		// Translate down so floor vertices go underground and ceiling is at y=0.
		double vertexWithPreScaleTranslationXs[stepCount];
		double vertexWithPreScaleTranslationYs[stepCount];
		double vertexWithPreScaleTranslationZs[stepCount];
		double vertexWithPreScaleTranslationWs[stepCount];
		Double4_AddN<stepCount>(vertexXs, vertexYs, vertexZs, vertexWs,
			g_meshProcessCaches.preScaleTranslationXs + meshIndex, g_meshProcessCaches.preScaleTranslationYs + meshIndex, g_meshProcessCaches.preScaleTranslationZs + meshIndex, preScaleTranslationWs,
			vertexWithPreScaleTranslationXs, vertexWithPreScaleTranslationYs, vertexWithPreScaleTranslationZs, vertexWithPreScaleTranslationWs);

		// Shrink towards y=0 depending on anim percent and door min visible amount.
		double scaledVertexXs[stepCount];
		double scaledVertexYs[stepCount];
		double scaledVertexZs[stepCount];
		double scaledVertexWs[stepCount];
		Matrix4_MultiplyVectorN<stepCount>(
			g_meshProcessCaches.scaleMatrixXXs + meshIndex, g_meshProcessCaches.scaleMatrixXYs + meshIndex, g_meshProcessCaches.scaleMatrixXZs + meshIndex, g_meshProcessCaches.scaleMatrixXWs + meshIndex,
			g_meshProcessCaches.scaleMatrixYXs + meshIndex, g_meshProcessCaches.scaleMatrixYYs + meshIndex, g_meshProcessCaches.scaleMatrixYZs + meshIndex, g_meshProcessCaches.scaleMatrixYWs + meshIndex,
			g_meshProcessCaches.scaleMatrixZXs + meshIndex, g_meshProcessCaches.scaleMatrixZYs + meshIndex, g_meshProcessCaches.scaleMatrixZZs + meshIndex, g_meshProcessCaches.scaleMatrixZWs + meshIndex,
			g_meshProcessCaches.scaleMatrixWXs + meshIndex, g_meshProcessCaches.scaleMatrixWYs + meshIndex, g_meshProcessCaches.scaleMatrixWZs + meshIndex, g_meshProcessCaches.scaleMatrixWWs + meshIndex,
			vertexWithPreScaleTranslationXs, vertexWithPreScaleTranslationYs, vertexWithPreScaleTranslationZs, vertexWithPreScaleTranslationWs,
			scaledVertexXs, scaledVertexYs, scaledVertexZs, scaledVertexWs);

		// Translate up to new model space Y position.
		double resultVertexXs[stepCount];
		double resultVertexYs[stepCount];
		double resultVertexZs[stepCount];
		double resultVertexWs[stepCount];
		Double4_SubtractN<stepCount>(scaledVertexXs, scaledVertexYs, scaledVertexZs, scaledVertexWs,
			g_meshProcessCaches.preScaleTranslationXs + meshIndex, g_meshProcessCaches.preScaleTranslationYs + meshIndex, g_meshProcessCaches.preScaleTranslationZs + meshIndex, preScaleTranslationWs,
			resultVertexXs, resultVertexYs, resultVertexZs, resultVertexWs);

		// Apply rotation matrix.
		double rotatedResultVertexXs[stepCount];
		double rotatedResultVertexYs[stepCount];
		double rotatedResultVertexZs[stepCount];
		double rotatedResultVertexWs[stepCount];
		Matrix4_MultiplyVectorN<stepCount>(
			g_meshProcessCaches.rotationMatrixXXs + meshIndex, g_meshProcessCaches.rotationMatrixXYs + meshIndex, g_meshProcessCaches.rotationMatrixXZs + meshIndex, g_meshProcessCaches.rotationMatrixXWs + meshIndex,
			g_meshProcessCaches.rotationMatrixYXs + meshIndex, g_meshProcessCaches.rotationMatrixYYs + meshIndex, g_meshProcessCaches.rotationMatrixYZs + meshIndex, g_meshProcessCaches.rotationMatrixYWs + meshIndex,
			g_meshProcessCaches.rotationMatrixZXs + meshIndex, g_meshProcessCaches.rotationMatrixZYs + meshIndex, g_meshProcessCaches.rotationMatrixZZs + meshIndex, g_meshProcessCaches.rotationMatrixZWs + meshIndex,
			g_meshProcessCaches.rotationMatrixWXs + meshIndex, g_meshProcessCaches.rotationMatrixWYs + meshIndex, g_meshProcessCaches.rotationMatrixWZs + meshIndex, g_meshProcessCaches.rotationMatrixWWs + meshIndex,
			resultVertexXs, resultVertexYs, resultVertexZs, resultVertexWs,
			rotatedResultVertexXs, rotatedResultVertexYs, rotatedResultVertexZs, rotatedResultVertexWs);

		// Apply translation matrix.
		double translatedResultVertexXs[stepCount];
		double translatedResultVertexYs[stepCount];
		double translatedResultVertexZs[stepCount];
		double translatedResultVertexWs[stepCount];
		Matrix4_MultiplyVectorN<stepCount>(
			g_meshProcessCaches.translationMatrixXXs + meshIndex, g_meshProcessCaches.translationMatrixXYs + meshIndex, g_meshProcessCaches.translationMatrixXZs + meshIndex, g_meshProcessCaches.translationMatrixXWs + meshIndex,
			g_meshProcessCaches.translationMatrixYXs + meshIndex, g_meshProcessCaches.translationMatrixYYs + meshIndex, g_meshProcessCaches.translationMatrixYZs + meshIndex, g_meshProcessCaches.translationMatrixYWs + meshIndex,
			g_meshProcessCaches.translationMatrixZXs + meshIndex, g_meshProcessCaches.translationMatrixZYs + meshIndex, g_meshProcessCaches.translationMatrixZZs + meshIndex, g_meshProcessCaches.translationMatrixZWs + meshIndex,
			g_meshProcessCaches.translationMatrixWXs + meshIndex, g_meshProcessCaches.translationMatrixWYs + meshIndex, g_meshProcessCaches.translationMatrixWZs + meshIndex, g_meshProcessCaches.translationMatrixWWs + meshIndex,
			rotatedResultVertexXs, rotatedResultVertexYs, rotatedResultVertexZs, rotatedResultVertexWs,
			translatedResultVertexXs, translatedResultVertexYs, translatedResultVertexZs, translatedResultVertexWs);

		// Apply view-projection matrix.
		Matrix4_MultiplyVectorN<stepCount>(
			g_viewProjMatrixXX, g_viewProjMatrixXY, g_viewProjMatrixXZ, g_viewProjMatrixXW,
			g_viewProjMatrixYX, g_viewProjMatrixYY, g_viewProjMatrixYZ, g_viewProjMatrixYW,
			g_viewProjMatrixZX, g_viewProjMatrixZY, g_viewProjMatrixZZ, g_viewProjMatrixZW,
			g_viewProjMatrixWX, g_viewProjMatrixWY, g_viewProjMatrixWZ, g_viewProjMatrixWW,
			translatedResultVertexXs, translatedResultVertexYs, translatedResultVertexZs, translatedResultVertexWs,
			outVertexXs, outVertexYs, outVertexZs, outVertexWs);
	}

	template<int stepCount>
	void VertexShader_Entity(int meshIndex, const double *vertexXs, const double *vertexYs, const double *vertexZs, const double *vertexWs,
		double *outVertexXs, double *outVertexYs, double *outVertexZs, double *outVertexWs)
	{
		// Apply model-view-projection matrix.
		Matrix4_MultiplyVectorN<stepCount>(
			g_meshProcessCaches.modelViewProjMatrixXXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXWs + meshIndex,
			g_meshProcessCaches.modelViewProjMatrixYXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYWs + meshIndex,
			g_meshProcessCaches.modelViewProjMatrixZXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZWs + meshIndex,
			g_meshProcessCaches.modelViewProjMatrixWXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWWs + meshIndex,
			vertexXs, vertexYs, vertexZs, vertexWs,
			outVertexXs, outVertexYs, outVertexZs, outVertexWs);
	}

	void PixelShader_Opaque(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		int texelX = -1;
		int texelY = -1;
		if (texture.samplingType == TextureSamplingType::Default)
		{
			texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
			texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		}
		else if (texture.samplingType == TextureSamplingType::ScreenSpaceRepeatY)
		{
			// @todo chasms: determine how many pixels the original texture should cover, based on what percentage the original texture height is over the original screen height.
			texelX = std::clamp(static_cast<int>(frameBuffer.xPercent * texture.widthReal), 0, texture.widthMinusOne);

			const double v = frameBuffer.yPercent * 2.0;
			const double actualV = v >= 1.0 ? (v - 1.0) : v;
			texelY = std::clamp(static_cast<int>(actualV * texture.heightReal), 0, texture.heightMinusOne);
		}

		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = shadedTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_OpaqueWithAlphaTestLayer(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &opaqueTexture,
		const PixelShaderTexture &alphaTestTexture, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int layerTexelX = std::clamp(static_cast<int>(perspective.texelPercent.x * alphaTestTexture.widthReal), 0, alphaTestTexture.widthMinusOne);
		const int layerTexelY = std::clamp(static_cast<int>(perspective.texelPercent.y * alphaTestTexture.heightReal), 0, alphaTestTexture.heightMinusOne);
		const int layerTexelIndex = layerTexelX + (layerTexelY * alphaTestTexture.width);
		uint8_t texel = alphaTestTexture.texels[layerTexelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			const int texelX = std::clamp(static_cast<int>(frameBuffer.xPercent * opaqueTexture.widthReal), 0, opaqueTexture.widthMinusOne);

			const double v = frameBuffer.yPercent * 2.0;
			const double actualV = v >= 1.0 ? (v - 1.0) : v;
			const int texelY = std::clamp(static_cast<int>(actualV * opaqueTexture.heightReal), 0, opaqueTexture.heightMinusOne);

			const int texelIndex = texelX + (texelY * opaqueTexture.width);
			texel = opaqueTexture.texels[texelIndex];
		}

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = shadedTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTested(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = shadedTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithVariableTexCoordUMin(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		double uMin, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const double u = std::clamp(uMin + ((1.0 - uMin) * perspective.texelPercent.x), uMin, 1.0);
		const int texelX = std::clamp(static_cast<int>(u * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.height), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = shadedTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithVariableTexCoordVMin(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		double vMin, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const double v = std::clamp(vMin + ((1.0 - vMin) * perspective.texelPercent.y), vMin, 1.0);
		const int texelY = std::clamp(static_cast<int>(v * texture.heightReal), 0, texture.heightMinusOne);

		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = shadedTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithPaletteIndexLookup(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderTexture &lookupTexture, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const uint8_t replacementTexel = lookupTexture.texels[texel];

		const int shadedTexelIndex = replacementTexel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = shadedTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithLightLevelColor(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int lightTableTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t resultTexel = lighting.lightTableTexels[lightTableTexelIndex];

		frameBuffer.colors[frameBuffer.pixelIndex] = resultTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithLightLevelOpacity(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		int lightTableTexelIndex;
		if (ArenaRenderUtils::isLightLevelTexel(texel))
		{
			const int lightLevel = static_cast<int>(texel) - ArenaRenderUtils::PALETTE_INDEX_LIGHT_LEVEL_LOWEST;
			const uint8_t prevFrameBufferPixel = frameBuffer.colors[frameBuffer.pixelIndex];
			lightTableTexelIndex = prevFrameBufferPixel + (lightLevel * lighting.texelsPerLightLevel);
		}
		else
		{
			const int lightTableOffset = lighting.lightLevel * lighting.texelsPerLightLevel;
			if (texel == ArenaRenderUtils::PALETTE_INDEX_LIGHT_LEVEL_SRC1)
			{
				lightTableTexelIndex = lightTableOffset + ArenaRenderUtils::PALETTE_INDEX_LIGHT_LEVEL_DST1;
			}
			else if (texel == ArenaRenderUtils::PALETTE_INDEX_LIGHT_LEVEL_SRC2)
			{
				lightTableTexelIndex = lightTableOffset + ArenaRenderUtils::PALETTE_INDEX_LIGHT_LEVEL_DST2;
			}
			else
			{
				lightTableTexelIndex = lightTableOffset + texel;
			}
		}

		const uint8_t resultTexel = lighting.lightTableTexels[lightTableTexelIndex];
		frameBuffer.colors[frameBuffer.pixelIndex] = resultTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithPreviousBrightnessLimit(const PixelShaderPerspectiveCorrection &perspective,
		const PixelShaderTexture &texture, PixelShaderFrameBuffer &frameBuffer)
	{
		constexpr int brightnessLimit = 0x3F; // Highest value each RGB component can be.
		constexpr uint8_t brightnessMask = ~brightnessLimit;
		constexpr uint32_t brightnessMaskR = brightnessMask << 16;
		constexpr uint32_t brightnessMaskG = brightnessMask << 8;
		constexpr uint32_t brightnessMaskB = brightnessMask;
		constexpr uint32_t brightnessMaskRGB = brightnessMaskR | brightnessMaskG | brightnessMaskB;

		const uint8_t prevFrameBufferPixel = frameBuffer.colors[frameBuffer.pixelIndex];
		const uint32_t prevFrameBufferColor = frameBuffer.palette.colors[prevFrameBufferPixel];
		const bool isDarkEnough = (prevFrameBufferColor & brightnessMaskRGB) == 0;
		if (!isDarkEnough)
		{
			return;
		}

		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		frameBuffer.colors[frameBuffer.pixelIndex] = texel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	void PixelShader_AlphaTestedWithHorizonMirror(const PixelShaderPerspectiveCorrection &perspective,
		const PixelShaderTexture &texture, const PixelShaderHorizonMirror &horizon, const PixelShaderLighting &lighting,
		PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercent.x * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercent.y * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		uint8_t resultTexel;
		const bool isReflective = texel == ArenaRenderUtils::PALETTE_INDEX_PUDDLE_EVEN_ROW;
		if (isReflective)
		{
			if (horizon.isReflectedPixelInFrameBuffer)
			{
				const uint8_t mirroredTexel = frameBuffer.colors[horizon.reflectedPixelIndex];
				resultTexel = mirroredTexel;
			}
			else
			{
				resultTexel = horizon.fallbackSkyColor;
			}
		}
		else
		{
			const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
			resultTexel = lighting.lightTableTexels[shadedTexelIndex];
		}

		frameBuffer.colors[frameBuffer.pixelIndex] = resultTexel;

		if (frameBuffer.enableDepthWrite)
		{
			frameBuffer.depth[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	// Internal geometry processing functions.
	// One per group of mesh process caches, for improving number crunching efficiency with vertex shading by
	// keeping the triangle count much higher than the average 2 per draw call.
	struct VertexShadingCache
	{
		Double4 unshadedV0s[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		Double4 unshadedV1s[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		Double4 unshadedV2s[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		Double2 uv0s[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		Double2 uv1s[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		Double2 uv2s[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		int meshProcessCacheIndices[MAX_VERTEX_SHADING_CACHE_TRIANGLES]; // Each triangle's mesh process cache it belongs to.
		int triangleCount;
	};

	VertexShadingCache g_vertexShadingCache;

	int g_totalClipSpaceTriangleCount = 0; // All processed triangles in the frustum, including new ones generated by clipping.
	int g_totalDrawCallTriangleCount = 0; // World space triangles generated by iterating index buffers. Doesn't include ones generated by clipping.

	void ClearTriangleTotalCounts()
	{
		// Skip zeroing mesh process caches for performance.
		g_totalClipSpaceTriangleCount = 0;
		g_totalDrawCallTriangleCount = 0;
	}

	// Handles the vertex/attribute/index buffer lookups for more efficient processing later.
	void ProcessMeshBufferLookups(int meshCount)
	{
		g_vertexShadingCache.triangleCount = 0;

		// Append vertices and texture coordinates into big arrays. The incoming meshes are likely tiny like 2 triangles each,
		// so this makes the total triangle loop longer for ease of number crunching.
		for (int meshIndex = 0; meshIndex < meshCount; meshIndex++)
		{
			const double *verticesPtr = g_meshProcessCaches.vertexBuffers[meshIndex]->vertices.begin();
			const double *texCoordsPtr = g_meshProcessCaches.texCoordBuffers[meshIndex]->attributes.begin();
			const SoftwareRenderer::IndexBuffer &indexBuffer = *g_meshProcessCaches.indexBuffers[meshIndex];
			const int32_t *indicesPtr = indexBuffer.indices.begin();
			const int triangleCount = indexBuffer.triangleCount;
			DebugAssert(triangleCount <= MAX_DRAW_CALL_MESH_TRIANGLES);

			int writeIndex = g_vertexShadingCache.triangleCount;
			for (int triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++)
			{
				const int indexBufferBase = triangleIndex * 3;
				const int32_t index0 = indicesPtr[indexBufferBase];
				const int32_t index1 = indicesPtr[indexBufferBase + 1];
				const int32_t index2 = indicesPtr[indexBufferBase + 2];

				const int32_t v0Index = index0 * 3;
				const int32_t v1Index = index1 * 3;
				const int32_t v2Index = index2 * 3;
				const Double3 unshadedV0(
					*(verticesPtr + v0Index),
					*(verticesPtr + v0Index + 1),
					*(verticesPtr + v0Index + 2));
				const Double3 unshadedV1(
					*(verticesPtr + v1Index),
					*(verticesPtr + v1Index + 1),
					*(verticesPtr + v1Index + 2));
				const Double3 unshadedV2(
					*(verticesPtr + v2Index),
					*(verticesPtr + v2Index + 1),
					*(verticesPtr + v2Index + 2));

				const int32_t uv0Index = index0 * 2;
				const int32_t uv1Index = index1 * 2;
				const int32_t uv2Index = index2 * 2;
				const Double2 uv0(
					*(texCoordsPtr + uv0Index),
					*(texCoordsPtr + uv0Index + 1));
				const Double2 uv1(
					*(texCoordsPtr + uv1Index),
					*(texCoordsPtr + uv1Index + 1));
				const Double2 uv2(
					*(texCoordsPtr + uv2Index),
					*(texCoordsPtr + uv2Index + 1));

				DebugAssert(writeIndex < MAX_VERTEX_SHADING_CACHE_TRIANGLES);
				g_vertexShadingCache.unshadedV0s[writeIndex] = Double4(unshadedV0, 1.0);
				g_vertexShadingCache.unshadedV1s[writeIndex] = Double4(unshadedV1, 1.0);
				g_vertexShadingCache.unshadedV2s[writeIndex] = Double4(unshadedV2, 1.0);
				g_vertexShadingCache.uv0s[writeIndex] = uv0;
				g_vertexShadingCache.uv1s[writeIndex] = uv1;
				g_vertexShadingCache.uv2s[writeIndex] = uv2;
				g_vertexShadingCache.meshProcessCacheIndices[writeIndex] = meshIndex;
				writeIndex++;
			}

			g_vertexShadingCache.triangleCount += triangleCount;
		}
	}

	void CalculateVertexShaderTransforms(int meshCount)
	{
		constexpr int stepCount = TYPICAL_STEP_COUNT;
		static_assert(stepCount <= MAX_MESH_PROCESS_CACHES);

		double rotationScaleMatrixXXs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixXYs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixXZs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixXWs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixYXs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixYYs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixYZs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixYWs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixZXs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixZYs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixZZs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixZWs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixWXs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixWYs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixWZs[MAX_MESH_PROCESS_CACHES];
		double rotationScaleMatrixWWs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixXXs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixXYs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixXZs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixXWs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixYXs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixYYs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixYZs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixYWs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixZXs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixZYs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixZZs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixZWs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixWXs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixWYs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixWZs[MAX_MESH_PROCESS_CACHES];
		double modelMatrixWWs[MAX_MESH_PROCESS_CACHES];

		const int meshCountStepAdjusted = meshCount - (stepCount - 1);
		int meshIndex = 0;
		while (meshIndex < meshCountStepAdjusted)
		{
			// Rotation-scale matrix
			Matrix4_MultiplyMatrixN<stepCount>(
				g_meshProcessCaches.rotationMatrixXXs + meshIndex, g_meshProcessCaches.rotationMatrixXYs + meshIndex, g_meshProcessCaches.rotationMatrixXZs + meshIndex, g_meshProcessCaches.rotationMatrixXWs + meshIndex,
				g_meshProcessCaches.rotationMatrixYXs + meshIndex, g_meshProcessCaches.rotationMatrixYYs + meshIndex, g_meshProcessCaches.rotationMatrixYZs + meshIndex, g_meshProcessCaches.rotationMatrixYWs + meshIndex,
				g_meshProcessCaches.rotationMatrixZXs + meshIndex, g_meshProcessCaches.rotationMatrixZYs + meshIndex, g_meshProcessCaches.rotationMatrixZZs + meshIndex, g_meshProcessCaches.rotationMatrixZWs + meshIndex,
				g_meshProcessCaches.rotationMatrixWXs + meshIndex, g_meshProcessCaches.rotationMatrixWYs + meshIndex, g_meshProcessCaches.rotationMatrixWZs + meshIndex, g_meshProcessCaches.rotationMatrixWWs + meshIndex,
				g_meshProcessCaches.scaleMatrixXXs + meshIndex, g_meshProcessCaches.scaleMatrixXYs + meshIndex, g_meshProcessCaches.scaleMatrixXZs + meshIndex, g_meshProcessCaches.scaleMatrixXWs + meshIndex,
				g_meshProcessCaches.scaleMatrixYXs + meshIndex, g_meshProcessCaches.scaleMatrixYYs + meshIndex, g_meshProcessCaches.scaleMatrixYZs + meshIndex, g_meshProcessCaches.scaleMatrixYWs + meshIndex,
				g_meshProcessCaches.scaleMatrixZXs + meshIndex, g_meshProcessCaches.scaleMatrixZYs + meshIndex, g_meshProcessCaches.scaleMatrixZZs + meshIndex, g_meshProcessCaches.scaleMatrixZWs + meshIndex,
				g_meshProcessCaches.scaleMatrixWXs + meshIndex, g_meshProcessCaches.scaleMatrixWYs + meshIndex, g_meshProcessCaches.scaleMatrixWZs + meshIndex, g_meshProcessCaches.scaleMatrixWWs + meshIndex,
				rotationScaleMatrixXXs + meshIndex, rotationScaleMatrixXYs + meshIndex, rotationScaleMatrixXZs + meshIndex, rotationScaleMatrixXWs + meshIndex,
				rotationScaleMatrixYXs + meshIndex, rotationScaleMatrixYYs + meshIndex, rotationScaleMatrixYZs + meshIndex, rotationScaleMatrixYWs + meshIndex,
				rotationScaleMatrixZXs + meshIndex, rotationScaleMatrixZYs + meshIndex, rotationScaleMatrixZZs + meshIndex, rotationScaleMatrixZWs + meshIndex,
				rotationScaleMatrixWXs + meshIndex, rotationScaleMatrixWYs + meshIndex, rotationScaleMatrixWZs + meshIndex, rotationScaleMatrixWWs + meshIndex);

			// Model matrix
			Matrix4_MultiplyMatrixN<stepCount>(
				g_meshProcessCaches.translationMatrixXXs + meshIndex, g_meshProcessCaches.translationMatrixXYs + meshIndex, g_meshProcessCaches.translationMatrixXZs + meshIndex, g_meshProcessCaches.translationMatrixXWs + meshIndex,
				g_meshProcessCaches.translationMatrixYXs + meshIndex, g_meshProcessCaches.translationMatrixYYs + meshIndex, g_meshProcessCaches.translationMatrixYZs + meshIndex, g_meshProcessCaches.translationMatrixYWs + meshIndex,
				g_meshProcessCaches.translationMatrixZXs + meshIndex, g_meshProcessCaches.translationMatrixZYs + meshIndex, g_meshProcessCaches.translationMatrixZZs + meshIndex, g_meshProcessCaches.translationMatrixZWs + meshIndex,
				g_meshProcessCaches.translationMatrixWXs + meshIndex, g_meshProcessCaches.translationMatrixWYs + meshIndex, g_meshProcessCaches.translationMatrixWZs + meshIndex, g_meshProcessCaches.translationMatrixWWs + meshIndex,
				rotationScaleMatrixXXs + meshIndex, rotationScaleMatrixXYs + meshIndex, rotationScaleMatrixXZs + meshIndex, rotationScaleMatrixXWs + meshIndex,
				rotationScaleMatrixYXs + meshIndex, rotationScaleMatrixYYs + meshIndex, rotationScaleMatrixYZs + meshIndex, rotationScaleMatrixYWs + meshIndex,
				rotationScaleMatrixZXs + meshIndex, rotationScaleMatrixZYs + meshIndex, rotationScaleMatrixZZs + meshIndex, rotationScaleMatrixZWs + meshIndex,
				rotationScaleMatrixWXs + meshIndex, rotationScaleMatrixWYs + meshIndex, rotationScaleMatrixWZs + meshIndex, rotationScaleMatrixWWs + meshIndex,
				modelMatrixXXs + meshIndex, modelMatrixXYs + meshIndex, modelMatrixXZs + meshIndex, modelMatrixXWs + meshIndex,
				modelMatrixYXs + meshIndex, modelMatrixYYs + meshIndex, modelMatrixYZs + meshIndex, modelMatrixYWs + meshIndex,
				modelMatrixZXs + meshIndex, modelMatrixZYs + meshIndex, modelMatrixZZs + meshIndex, modelMatrixZWs + meshIndex,
				modelMatrixWXs + meshIndex, modelMatrixWYs + meshIndex, modelMatrixWZs + meshIndex, modelMatrixWWs + meshIndex);

			// Model-view-projection matrix
			Matrix4_MultiplyMatrixN<stepCount>(
				g_viewProjMatrixXX, g_viewProjMatrixXY, g_viewProjMatrixXZ, g_viewProjMatrixXW,
				g_viewProjMatrixYX, g_viewProjMatrixYY, g_viewProjMatrixYZ, g_viewProjMatrixYW,
				g_viewProjMatrixZX, g_viewProjMatrixZY, g_viewProjMatrixZZ, g_viewProjMatrixZW,
				g_viewProjMatrixWX, g_viewProjMatrixWY, g_viewProjMatrixWZ, g_viewProjMatrixWW,
				modelMatrixXXs + meshIndex, modelMatrixXYs + meshIndex, modelMatrixXZs + meshIndex, modelMatrixXWs + meshIndex,
				modelMatrixYXs + meshIndex, modelMatrixYYs + meshIndex, modelMatrixYZs + meshIndex, modelMatrixYWs + meshIndex,
				modelMatrixZXs + meshIndex, modelMatrixZYs + meshIndex, modelMatrixZZs + meshIndex, modelMatrixZWs + meshIndex,
				modelMatrixWXs + meshIndex, modelMatrixWYs + meshIndex, modelMatrixWZs + meshIndex, modelMatrixWWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixXXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixYXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixZXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixWXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWWs + meshIndex);

			meshIndex += stepCount;
		}

		while (meshIndex < meshCount)
		{
			// Rotation-scale matrix
			Matrix4_MultiplyMatrixN<1>(
				g_meshProcessCaches.rotationMatrixXXs + meshIndex, g_meshProcessCaches.rotationMatrixXYs + meshIndex, g_meshProcessCaches.rotationMatrixXZs + meshIndex, g_meshProcessCaches.rotationMatrixXWs + meshIndex,
				g_meshProcessCaches.rotationMatrixYXs + meshIndex, g_meshProcessCaches.rotationMatrixYYs + meshIndex, g_meshProcessCaches.rotationMatrixYZs + meshIndex, g_meshProcessCaches.rotationMatrixYWs + meshIndex,
				g_meshProcessCaches.rotationMatrixZXs + meshIndex, g_meshProcessCaches.rotationMatrixZYs + meshIndex, g_meshProcessCaches.rotationMatrixZZs + meshIndex, g_meshProcessCaches.rotationMatrixZWs + meshIndex,
				g_meshProcessCaches.rotationMatrixWXs + meshIndex, g_meshProcessCaches.rotationMatrixWYs + meshIndex, g_meshProcessCaches.rotationMatrixWZs + meshIndex, g_meshProcessCaches.rotationMatrixWWs + meshIndex,
				g_meshProcessCaches.scaleMatrixXXs + meshIndex, g_meshProcessCaches.scaleMatrixXYs + meshIndex, g_meshProcessCaches.scaleMatrixXZs + meshIndex, g_meshProcessCaches.scaleMatrixXWs + meshIndex,
				g_meshProcessCaches.scaleMatrixYXs + meshIndex, g_meshProcessCaches.scaleMatrixYYs + meshIndex, g_meshProcessCaches.scaleMatrixYZs + meshIndex, g_meshProcessCaches.scaleMatrixYWs + meshIndex,
				g_meshProcessCaches.scaleMatrixZXs + meshIndex, g_meshProcessCaches.scaleMatrixZYs + meshIndex, g_meshProcessCaches.scaleMatrixZZs + meshIndex, g_meshProcessCaches.scaleMatrixZWs + meshIndex,
				g_meshProcessCaches.scaleMatrixWXs + meshIndex, g_meshProcessCaches.scaleMatrixWYs + meshIndex, g_meshProcessCaches.scaleMatrixWZs + meshIndex, g_meshProcessCaches.scaleMatrixWWs + meshIndex,
				rotationScaleMatrixXXs + meshIndex, rotationScaleMatrixXYs + meshIndex, rotationScaleMatrixXZs + meshIndex, rotationScaleMatrixXWs + meshIndex,
				rotationScaleMatrixYXs + meshIndex, rotationScaleMatrixYYs + meshIndex, rotationScaleMatrixYZs + meshIndex, rotationScaleMatrixYWs + meshIndex,
				rotationScaleMatrixZXs + meshIndex, rotationScaleMatrixZYs + meshIndex, rotationScaleMatrixZZs + meshIndex, rotationScaleMatrixZWs + meshIndex,
				rotationScaleMatrixWXs + meshIndex, rotationScaleMatrixWYs + meshIndex, rotationScaleMatrixWZs + meshIndex, rotationScaleMatrixWWs + meshIndex);

			// Model matrix
			Matrix4_MultiplyMatrixN<1>(
				g_meshProcessCaches.translationMatrixXXs + meshIndex, g_meshProcessCaches.translationMatrixXYs + meshIndex, g_meshProcessCaches.translationMatrixXZs + meshIndex, g_meshProcessCaches.translationMatrixXWs + meshIndex,
				g_meshProcessCaches.translationMatrixYXs + meshIndex, g_meshProcessCaches.translationMatrixYYs + meshIndex, g_meshProcessCaches.translationMatrixYZs + meshIndex, g_meshProcessCaches.translationMatrixYWs + meshIndex,
				g_meshProcessCaches.translationMatrixZXs + meshIndex, g_meshProcessCaches.translationMatrixZYs + meshIndex, g_meshProcessCaches.translationMatrixZZs + meshIndex, g_meshProcessCaches.translationMatrixZWs + meshIndex,
				g_meshProcessCaches.translationMatrixWXs + meshIndex, g_meshProcessCaches.translationMatrixWYs + meshIndex, g_meshProcessCaches.translationMatrixWZs + meshIndex, g_meshProcessCaches.translationMatrixWWs + meshIndex,
				rotationScaleMatrixXXs + meshIndex, rotationScaleMatrixXYs + meshIndex, rotationScaleMatrixXZs + meshIndex, rotationScaleMatrixXWs + meshIndex,
				rotationScaleMatrixYXs + meshIndex, rotationScaleMatrixYYs + meshIndex, rotationScaleMatrixYZs + meshIndex, rotationScaleMatrixYWs + meshIndex,
				rotationScaleMatrixZXs + meshIndex, rotationScaleMatrixZYs + meshIndex, rotationScaleMatrixZZs + meshIndex, rotationScaleMatrixZWs + meshIndex,
				rotationScaleMatrixWXs + meshIndex, rotationScaleMatrixWYs + meshIndex, rotationScaleMatrixWZs + meshIndex, rotationScaleMatrixWWs + meshIndex,
				modelMatrixXXs + meshIndex, modelMatrixXYs + meshIndex, modelMatrixXZs + meshIndex, modelMatrixXWs + meshIndex,
				modelMatrixYXs + meshIndex, modelMatrixYYs + meshIndex, modelMatrixYZs + meshIndex, modelMatrixYWs + meshIndex,
				modelMatrixZXs + meshIndex, modelMatrixZYs + meshIndex, modelMatrixZZs + meshIndex, modelMatrixZWs + meshIndex,
				modelMatrixWXs + meshIndex, modelMatrixWYs + meshIndex, modelMatrixWZs + meshIndex, modelMatrixWWs + meshIndex);

			// Model-view-projection matrix
			Matrix4_MultiplyMatrixN<1>(
				g_viewProjMatrixXX, g_viewProjMatrixXY, g_viewProjMatrixXZ, g_viewProjMatrixXW,
				g_viewProjMatrixYX, g_viewProjMatrixYY, g_viewProjMatrixYZ, g_viewProjMatrixYW,
				g_viewProjMatrixZX, g_viewProjMatrixZY, g_viewProjMatrixZZ, g_viewProjMatrixZW,
				g_viewProjMatrixWX, g_viewProjMatrixWY, g_viewProjMatrixWZ, g_viewProjMatrixWW,
				modelMatrixXXs + meshIndex, modelMatrixXYs + meshIndex, modelMatrixXZs + meshIndex, modelMatrixXWs + meshIndex,
				modelMatrixYXs + meshIndex, modelMatrixYYs + meshIndex, modelMatrixYZs + meshIndex, modelMatrixYWs + meshIndex,
				modelMatrixZXs + meshIndex, modelMatrixZYs + meshIndex, modelMatrixZZs + meshIndex, modelMatrixZWs + meshIndex,
				modelMatrixWXs + meshIndex, modelMatrixWYs + meshIndex, modelMatrixWZs + meshIndex, modelMatrixWWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixXXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixXWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixYXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixYWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixZXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixZWs + meshIndex,
				g_meshProcessCaches.modelViewProjMatrixWXs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWYs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWZs + meshIndex, g_meshProcessCaches.modelViewProjMatrixWWs + meshIndex);

			meshIndex++;
		}
	}

	// Converts several meshes' world space vertices to clip space.
	template<VertexShaderType vertexShaderType>
	void ProcessVertexShadersInternal(int meshCount)
	{
		for (int meshIndex = 0; meshIndex < meshCount; meshIndex++)
		{
			g_meshProcessCaches.triangleWriteCounts[meshIndex] = 0;
		}

		// Run vertex shaders on each triangle and store the results for clipping.
		for (int triangleIndex = 0; triangleIndex < g_vertexShadingCache.triangleCount; triangleIndex++)
		{
			const int meshIndex = g_vertexShadingCache.meshProcessCacheIndices[triangleIndex];
			const Double4 unshadedV0 = g_vertexShadingCache.unshadedV0s[triangleIndex];
			const Double4 unshadedV1 = g_vertexShadingCache.unshadedV1s[triangleIndex];
			const Double4 unshadedV2 = g_vertexShadingCache.unshadedV2s[triangleIndex];

			// @todo: use plain doubles arrays for all unshaded components etc. instead of this conversion step
			double unshadedV0Xs[1] = { unshadedV0.x };
			double unshadedV0Ys[1] = { unshadedV0.y };
			double unshadedV0Zs[1] = { unshadedV0.z };
			double unshadedV0Ws[1] = { unshadedV0.w };
			double unshadedV1Xs[1] = { unshadedV1.x };
			double unshadedV1Ys[1] = { unshadedV1.y };
			double unshadedV1Zs[1] = { unshadedV1.z };
			double unshadedV1Ws[1] = { unshadedV1.w };
			double unshadedV2Xs[1] = { unshadedV2.x };
			double unshadedV2Ys[1] = { unshadedV2.y };
			double unshadedV2Zs[1] = { unshadedV2.z };
			double unshadedV2Ws[1] = { unshadedV2.w };

			constexpr int stepCount = 1; // @todo: TYPICAL_STEP_COUNT
			double shadedV0Xs[stepCount] = { 0.0 };
			double shadedV0Ys[stepCount] = { 0.0 };
			double shadedV0Zs[stepCount] = { 0.0 };
			double shadedV0Ws[stepCount] = { 0.0 };
			double shadedV1Xs[stepCount] = { 0.0 };
			double shadedV1Ys[stepCount] = { 0.0 };
			double shadedV1Zs[stepCount] = { 0.0 };
			double shadedV1Ws[stepCount] = { 0.0 };
			double shadedV2Xs[stepCount] = { 0.0 };
			double shadedV2Ys[stepCount] = { 0.0 };
			double shadedV2Zs[stepCount] = { 0.0 };
			double shadedV2Ws[stepCount] = { 0.0 };
			if constexpr (vertexShaderType == VertexShaderType::Basic)
			{
				VertexShader_Basic<stepCount>(meshIndex, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_Basic<stepCount>(meshIndex, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_Basic<stepCount>(meshIndex, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}
			else if (vertexShaderType == VertexShaderType::RaisingDoor)
			{
				VertexShader_RaisingDoor<stepCount>(meshIndex, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_RaisingDoor<stepCount>(meshIndex, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_RaisingDoor<stepCount>(meshIndex, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}
			else if (vertexShaderType == VertexShaderType::Entity)
			{
				VertexShader_Entity<stepCount>(meshIndex, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_Entity<stepCount>(meshIndex, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_Entity<stepCount>(meshIndex, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}

			auto &shadedV0s = g_meshProcessCaches.shadedV0Arrays[meshIndex];
			auto &shadedV1s = g_meshProcessCaches.shadedV1Arrays[meshIndex];
			auto &shadedV2s = g_meshProcessCaches.shadedV2Arrays[meshIndex];
			auto &uv0s = g_meshProcessCaches.uv0Arrays[meshIndex];
			auto &uv1s = g_meshProcessCaches.uv1Arrays[meshIndex];
			auto &uv2s = g_meshProcessCaches.uv2Arrays[meshIndex];

			int &writeIndex = g_meshProcessCaches.triangleWriteCounts[meshIndex];
			DebugAssert(writeIndex < MAX_DRAW_CALL_MESH_TRIANGLES);
			shadedV0s[writeIndex].x = shadedV0Xs[0];
			shadedV0s[writeIndex].y = shadedV0Ys[0];
			shadedV0s[writeIndex].z = shadedV0Zs[0];
			shadedV0s[writeIndex].w = shadedV0Ws[0];
			shadedV1s[writeIndex].x = shadedV1Xs[0];
			shadedV1s[writeIndex].y = shadedV1Ys[0];
			shadedV1s[writeIndex].z = shadedV1Zs[0];
			shadedV1s[writeIndex].w = shadedV1Ws[0];
			shadedV2s[writeIndex].x = shadedV2Xs[0];
			shadedV2s[writeIndex].y = shadedV2Ys[0];
			shadedV2s[writeIndex].z = shadedV2Zs[0];
			shadedV2s[writeIndex].w = shadedV2Ws[0];
			uv0s[writeIndex] = g_vertexShadingCache.uv0s[triangleIndex];
			uv1s[writeIndex] = g_vertexShadingCache.uv1s[triangleIndex];
			uv2s[writeIndex] = g_vertexShadingCache.uv2s[triangleIndex];
			writeIndex++;
		}

		g_totalDrawCallTriangleCount += g_vertexShadingCache.triangleCount;
	}

	// Operates on the current sequence of draw call meshes with the chosen vertex shader then writes results
	// to a cache for mesh clipping.
	void ProcessVertexShaders(int meshCount, VertexShaderType vertexShaderType)
	{
		// Dispatch based on vertex shader.
		switch (vertexShaderType)
		{
		case VertexShaderType::Basic:
			ProcessVertexShadersInternal<VertexShaderType::Basic>(meshCount);
			break;
		case VertexShaderType::RaisingDoor:
			ProcessVertexShadersInternal<VertexShaderType::RaisingDoor>(meshCount);
			break;
		case VertexShaderType::Entity:
			ProcessVertexShadersInternal<VertexShaderType::Entity>(meshCount);
			break;
		default:
			DebugNotImplementedMsg(std::to_string(static_cast<int>(vertexShaderType)));
			break;
		}
	}

	// Clips triangles to the frustum then writes out clip space triangle indices for the rasterizer to iterate.
	void ProcessClipping(int meshCount)
	{
		for (int meshIndex = 0; meshIndex < meshCount; meshIndex++)
		{
			auto &shadedV0s = g_meshProcessCaches.shadedV0Arrays[meshIndex];
			auto &shadedV1s = g_meshProcessCaches.shadedV1Arrays[meshIndex];
			auto &shadedV2s = g_meshProcessCaches.shadedV2Arrays[meshIndex];
			auto &uv0s = g_meshProcessCaches.uv0Arrays[meshIndex];
			auto &uv1s = g_meshProcessCaches.uv1Arrays[meshIndex];
			auto &uv2s = g_meshProcessCaches.uv2Arrays[meshIndex];
			auto &clipSpaceTriangleV0s = g_meshProcessCaches.clipSpaceTriangleV0Arrays[meshIndex];
			auto &clipSpaceTriangleV1s = g_meshProcessCaches.clipSpaceTriangleV1Arrays[meshIndex];
			auto &clipSpaceTriangleV2s = g_meshProcessCaches.clipSpaceTriangleV2Arrays[meshIndex];
			auto &clipSpaceTriangleUV0s = g_meshProcessCaches.clipSpaceTriangleUV0Arrays[meshIndex];
			auto &clipSpaceTriangleUV1s = g_meshProcessCaches.clipSpaceTriangleUV1Arrays[meshIndex];
			auto &clipSpaceTriangleUV2s = g_meshProcessCaches.clipSpaceTriangleUV2Arrays[meshIndex];
			auto &clipSpaceMeshV0s = g_meshProcessCaches.clipSpaceMeshV0Arrays[meshIndex];
			auto &clipSpaceMeshV1s = g_meshProcessCaches.clipSpaceMeshV1Arrays[meshIndex];
			auto &clipSpaceMeshV2s = g_meshProcessCaches.clipSpaceMeshV2Arrays[meshIndex];
			auto &clipSpaceMeshUV0s = g_meshProcessCaches.clipSpaceMeshUV0Arrays[meshIndex];
			auto &clipSpaceMeshUV1s = g_meshProcessCaches.clipSpaceMeshUV1Arrays[meshIndex];
			auto &clipSpaceMeshUV2s = g_meshProcessCaches.clipSpaceMeshUV2Arrays[meshIndex];
			int &clipSpaceMeshTriangleCount = g_meshProcessCaches.clipSpaceMeshTriangleCounts[meshIndex];

			// Reset clip space cache. Skip zeroing the mesh arrays for performance.
			clipSpaceMeshTriangleCount = 0;

			// Clip each vertex-shaded triangle and save them in a cache for rasterization.
			const int triangleCount = g_meshProcessCaches.indexBuffers[meshIndex]->triangleCount;
			for (int triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++)
			{
				// Initialize clipping loop with the vertex-shaded triangle.
				clipSpaceTriangleV0s[0] = shadedV0s[triangleIndex];
				clipSpaceTriangleV1s[0] = shadedV1s[triangleIndex];
				clipSpaceTriangleV2s[0] = shadedV2s[triangleIndex];
				clipSpaceTriangleUV0s[0] = uv0s[triangleIndex];
				clipSpaceTriangleUV1s[0] = uv1s[triangleIndex];
				clipSpaceTriangleUV2s[0] = uv2s[triangleIndex];

				int clipListSize = 1; // Triangles to process based on this vertex-shaded triangle.
				int clipListFrontIndex = 0;

				constexpr int clipPlaneCount = 6; // Check each dimension against -W and W components.
				for (int clipPlaneIndex = 0; clipPlaneIndex < clipPlaneCount; clipPlaneIndex++)
				{
					const int trianglesToClipCount = clipListSize - clipListFrontIndex;
					for (int triangleToClip = trianglesToClipCount; triangleToClip > 0; triangleToClip--)
					{
						// Clip against the clipping plane, generating 0 to 2 triangles.
						const Double4 currentV0 = clipSpaceTriangleV0s[clipListFrontIndex];
						const Double4 currentV1 = clipSpaceTriangleV1s[clipListFrontIndex];
						const Double4 currentV2 = clipSpaceTriangleV2s[clipListFrontIndex];

						double v0Component, v1Component, v2Component;
						if ((clipPlaneIndex == 0) || (clipPlaneIndex == 1))
						{
							v0Component = currentV0.x;
							v1Component = currentV1.x;
							v2Component = currentV2.x;
						}
						else if ((clipPlaneIndex == 2) || (clipPlaneIndex == 3))
						{
							v0Component = currentV0.y;
							v1Component = currentV1.y;
							v2Component = currentV2.y;
						}
						else
						{
							v0Component = currentV0.z;
							v1Component = currentV1.z;
							v2Component = currentV2.z;
						}

						double v0w, v1w, v2w;
						double comparisonSign;
						if ((clipPlaneIndex & 1) == 0)
						{
							v0w = currentV0.w;
							v1w = currentV1.w;
							v2w = currentV2.w;
							comparisonSign = 1.0;
						}
						else
						{
							v0w = -currentV0.w;
							v1w = -currentV1.w;
							v2w = -currentV2.w;
							comparisonSign = -1.0;
						}

						const double v0Diff = v0Component + v0w;
						const double v1Diff = v1Component + v1w;
						const double v2Diff = v2Component + v2w;
						const bool isV0Inside = (v0Diff * comparisonSign) >= 0.0;
						const bool isV1Inside = (v1Diff * comparisonSign) >= 0.0;
						const bool isV2Inside = (v2Diff * comparisonSign) >= 0.0;

						const Double2 currentUV0 = clipSpaceTriangleUV0s[clipListFrontIndex];
						const Double2 currentUV1 = clipSpaceTriangleUV1s[clipListFrontIndex];
						const Double2 currentUV2 = clipSpaceTriangleUV2s[clipListFrontIndex];
						const int resultWriteIndex0 = clipListSize;
						const int resultWriteIndex1 = clipListSize + 1;

						// Determine which two line segments are intersecting the clipping plane and generate two new vertices,
						// making sure to keep the original winding order.
						int clipResultCount;
						const int insideMaskIndex = (isV2Inside ? 0 : 1) | (isV1Inside ? 0 : 2) | (isV0Inside ? 0 : 4);
						switch (insideMaskIndex)
						{
						case 0:
							// All vertices visible, no clipping needed.
							clipSpaceTriangleV0s[resultWriteIndex0] = currentV0;
							clipSpaceTriangleV1s[resultWriteIndex0] = currentV1;
							clipSpaceTriangleV2s[resultWriteIndex0] = currentV2;
							clipSpaceTriangleUV0s[resultWriteIndex0] = currentUV0;
							clipSpaceTriangleUV1s[resultWriteIndex0] = currentUV1;
							clipSpaceTriangleUV2s[resultWriteIndex0] = currentUV2;
							clipResultCount = 1;
							break;
						case 1:
						{
							// Becomes quad
							// Inside: V0, V1
							// Outside: V2
							const double v1v2PointT = v1Diff / (v1Diff - v2Diff);
							const double v2v0PointT = v2Diff / (v2Diff - v0Diff);
							const Double4 v1v2Point = currentV1.lerp(currentV2, v1v2PointT);
							const Double4 v2v0Point = currentV2.lerp(currentV0, v2v0PointT);
							const Double2 v1v2PointUV = currentUV1.lerp(currentUV2, v1v2PointT);
							const Double2 v2v0PointUV = currentUV2.lerp(currentUV0, v2v0PointT);
							clipSpaceTriangleV0s[resultWriteIndex0] = currentV0;
							clipSpaceTriangleV1s[resultWriteIndex0] = currentV1;
							clipSpaceTriangleV2s[resultWriteIndex0] = v1v2Point;
							clipSpaceTriangleV0s[resultWriteIndex1] = v1v2Point;
							clipSpaceTriangleV1s[resultWriteIndex1] = v2v0Point;
							clipSpaceTriangleV2s[resultWriteIndex1] = currentV0;
							clipSpaceTriangleUV0s[resultWriteIndex0] = currentUV0;
							clipSpaceTriangleUV1s[resultWriteIndex0] = currentUV1;
							clipSpaceTriangleUV2s[resultWriteIndex0] = v1v2PointUV;
							clipSpaceTriangleUV0s[resultWriteIndex1] = v1v2PointUV;
							clipSpaceTriangleUV1s[resultWriteIndex1] = v2v0PointUV;
							clipSpaceTriangleUV2s[resultWriteIndex1] = currentUV0;
							clipResultCount = 2;
							break;
						}
						case 2:
						{
							// Becomes quad
							// Inside: V0, V2
							// Outside: V1
							const double v0v1PointT = v0Diff / (v0Diff - v1Diff);
							const double v1v2PointT = v1Diff / (v1Diff - v2Diff);
							const Double4 v0v1Point = currentV0.lerp(currentV1, v0v1PointT);
							const Double4 v1v2Point = currentV1.lerp(currentV2, v1v2PointT);
							const Double2 v0v1PointUV = currentUV0.lerp(currentUV1, v0v1PointT);
							const Double2 v1v2PointUV = currentUV1.lerp(currentUV2, v1v2PointT);
							clipSpaceTriangleV0s[resultWriteIndex0] = currentV0;
							clipSpaceTriangleV1s[resultWriteIndex0] = v0v1Point;
							clipSpaceTriangleV2s[resultWriteIndex0] = v1v2Point;
							clipSpaceTriangleV0s[resultWriteIndex1] = v1v2Point;
							clipSpaceTriangleV1s[resultWriteIndex1] = currentV2;
							clipSpaceTriangleV2s[resultWriteIndex1] = currentV0;
							clipSpaceTriangleUV0s[resultWriteIndex0] = currentUV0;
							clipSpaceTriangleUV1s[resultWriteIndex0] = v0v1PointUV;
							clipSpaceTriangleUV2s[resultWriteIndex0] = v1v2PointUV;
							clipSpaceTriangleUV0s[resultWriteIndex1] = v1v2PointUV;
							clipSpaceTriangleUV1s[resultWriteIndex1] = currentUV2;
							clipSpaceTriangleUV2s[resultWriteIndex1] = currentUV0;
							clipResultCount = 2;
							break;
						}
						case 3:
						{
							// Becomes smaller triangle
							// Inside: V0
							// Outside: V1, V2
							const double v0v1PointT = v0Diff / (v0Diff - v1Diff);
							const double v2v0PointT = v2Diff / (v2Diff - v0Diff);
							const Double4 v0v1Point = currentV0.lerp(currentV1, v0v1PointT);
							const Double4 v2v0Point = currentV2.lerp(currentV0, v2v0PointT);
							const Double2 v0v1PointUV = currentUV0.lerp(currentUV1, v0v1PointT);
							const Double2 v2v0PointUV = currentUV2.lerp(currentUV0, v2v0PointT);
							clipSpaceTriangleV0s[resultWriteIndex0] = currentV0;
							clipSpaceTriangleV1s[resultWriteIndex0] = v0v1Point;
							clipSpaceTriangleV2s[resultWriteIndex0] = v2v0Point;
							clipSpaceTriangleUV0s[resultWriteIndex0] = currentUV0;
							clipSpaceTriangleUV1s[resultWriteIndex0] = v0v1PointUV;
							clipSpaceTriangleUV2s[resultWriteIndex0] = v2v0PointUV;
							clipResultCount = 1;
							break;
						}
						case 4:
						{
							// Becomes quad
							// Inside: V1, V2
							// Outside: V0
							const double v0v1PointT = v0Diff / (v0Diff - v1Diff);
							const double v2v0PointT = v2Diff / (v2Diff - v0Diff);
							const Double4 v0v1Point = currentV0.lerp(currentV1, v0v1PointT);
							const Double4 v2v0Point = currentV2.lerp(currentV0, v2v0PointT);
							const Double2 v0v1PointUV = currentUV0.lerp(currentUV1, v0v1PointT);
							const Double2 v2v0PointUV = currentUV2.lerp(currentUV0, v2v0PointT);
							clipSpaceTriangleV0s[resultWriteIndex0] = v0v1Point;
							clipSpaceTriangleV1s[resultWriteIndex0] = currentV1;
							clipSpaceTriangleV2s[resultWriteIndex0] = currentV2;
							clipSpaceTriangleV0s[resultWriteIndex1] = currentV2;
							clipSpaceTriangleV1s[resultWriteIndex1] = v2v0Point;
							clipSpaceTriangleV2s[resultWriteIndex1] = v0v1Point;
							clipSpaceTriangleUV0s[resultWriteIndex0] = v0v1PointUV;
							clipSpaceTriangleUV1s[resultWriteIndex0] = currentUV1;
							clipSpaceTriangleUV2s[resultWriteIndex0] = currentUV2;
							clipSpaceTriangleUV0s[resultWriteIndex1] = currentUV2;
							clipSpaceTriangleUV1s[resultWriteIndex1] = v2v0PointUV;
							clipSpaceTriangleUV2s[resultWriteIndex1] = v0v1PointUV;
							clipResultCount = 2;
							break;
						}
						case 5:
						{
							// Becomes smaller triangle
							// Inside: V1
							// Outside: V0, V2
							const double v0v1PointT = v0Diff / (v0Diff - v1Diff);
							const double v1v2PointT = v1Diff / (v1Diff - v2Diff);
							const Double4 v0v1Point = currentV0.lerp(currentV1, v0v1PointT);
							const Double4 v1v2Point = currentV1.lerp(currentV2, v1v2PointT);
							const Double2 v0v1PointUV = currentUV0.lerp(currentUV1, v0v1PointT);
							const Double2 v1v2PointUV = currentUV1.lerp(currentUV2, v1v2PointT);
							clipSpaceTriangleV0s[resultWriteIndex0] = v0v1Point;
							clipSpaceTriangleV1s[resultWriteIndex0] = currentV1;
							clipSpaceTriangleV2s[resultWriteIndex0] = v1v2Point;
							clipSpaceTriangleUV0s[resultWriteIndex0] = v0v1PointUV;
							clipSpaceTriangleUV1s[resultWriteIndex0] = currentUV1;
							clipSpaceTriangleUV2s[resultWriteIndex0] = v1v2PointUV;
							clipResultCount = 1;
							break;
						}
						case 6:
						{
							// Becomes smaller triangle
							// Inside: V2
							// Outside: V0, V1
							const double v1v2PointT = v1Diff / (v1Diff - v2Diff);
							const double v2v0PointT = v2Diff / (v2Diff - v0Diff);
							const Double4 v1v2Point = currentV1.lerp(currentV2, v1v2PointT);
							const Double4 v2v0Point = currentV2.lerp(currentV0, v2v0PointT);
							const Double2 v1v2PointUV = currentUV1.lerp(currentUV2, v1v2PointT);
							const Double2 v2v0PointUV = currentUV2.lerp(currentUV0, v2v0PointT);
							clipSpaceTriangleV0s[resultWriteIndex0] = v1v2Point;
							clipSpaceTriangleV1s[resultWriteIndex0] = currentV2;
							clipSpaceTriangleV2s[resultWriteIndex0] = v2v0Point;
							clipSpaceTriangleUV0s[resultWriteIndex0] = v1v2PointUV;
							clipSpaceTriangleUV1s[resultWriteIndex0] = currentUV2;
							clipSpaceTriangleUV2s[resultWriteIndex0] = v2v0PointUV;
							clipResultCount = 1;
							break;
						}
						case 7:
							// All vertices outside frustum.
							clipResultCount = 0;
							break;
						}

						clipListSize += clipResultCount;
						clipListFrontIndex++;
					}
				}

				// Add the clip results to the mesh, skipping the incomplete triangles the front index advanced beyond.
				const int resultTriangleCount = clipListSize - clipListFrontIndex;
				for (int resultTriangleIndex = 0; resultTriangleIndex < resultTriangleCount; resultTriangleIndex++)
				{
					const int srcIndex = clipListFrontIndex + resultTriangleIndex;
					const int dstIndex = clipSpaceMeshTriangleCount + resultTriangleIndex;
					clipSpaceMeshV0s[dstIndex] = clipSpaceTriangleV0s[srcIndex];
					clipSpaceMeshV1s[dstIndex] = clipSpaceTriangleV1s[srcIndex];
					clipSpaceMeshV2s[dstIndex] = clipSpaceTriangleV2s[srcIndex];
					clipSpaceMeshUV0s[dstIndex] = clipSpaceTriangleUV0s[srcIndex];
					clipSpaceMeshUV1s[dstIndex] = clipSpaceTriangleUV1s[srcIndex];
					clipSpaceMeshUV2s[dstIndex] = clipSpaceTriangleUV2s[srcIndex];
				}

				clipSpaceMeshTriangleCount += resultTriangleCount;
			}

			g_totalClipSpaceTriangleCount += clipSpaceMeshTriangleCount;
		}
	}

	// Rendering functions, per-pixel work.
	constexpr int DITHERING_MODE_NONE = 0;
	constexpr int DITHERING_MODE_CLASSIC = 1;
	constexpr int DITHERING_MODE_MODERN = 2;

	constexpr int DITHERING_MODE_MODERN_MASK_COUNT = 4;

	int g_totalDrawCallCount = 0;

	// For measuring overdraw.
	int g_totalDepthTests = 0;
	int g_totalColorWrites = 0;

	void CreateDitherBuffer(Buffer3D<bool> &ditherBuffer, int width, int height, int ditheringMode)
	{
		if (ditheringMode == DITHERING_MODE_CLASSIC)
		{
			// Original game: 2x2, top left + bottom right are darkened.
			ditherBuffer.init(width, height, 1);

			bool *ditherPixels = ditherBuffer.begin();
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					const bool shouldDither = ((x + y) & 0x1) == 0;
					const int index = x + (y * width);
					ditherPixels[index] = shouldDither;
				}
			}
		}
		else if (ditheringMode == DITHERING_MODE_MODERN)
		{
			// Modern 2x2, four levels of dither depending on percent between two light levels.
			ditherBuffer.init(width, height, DITHERING_MODE_MODERN_MASK_COUNT);
			static_assert(DITHERING_MODE_MODERN_MASK_COUNT == 4);

			bool *ditherPixels = ditherBuffer.begin();
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					const bool shouldDither0 = (((x + y) & 0x1) == 0) || (((x % 2) == 1) && ((y % 2) == 0)); // Top left, bottom right, top right
					const bool shouldDither1 = ((x + y) & 0x1) == 0; // Top left + bottom right
					const bool shouldDither2 = ((x % 2) == 0) && ((y % 2) == 0); // Top left
					const bool shouldDither3 = false;
					const int index0 = x + (y * width);
					const int index1 = x + (y * width) + (1 * width * height);
					const int index2 = x + (y * width) + (2 * width * height);
					const int index3 = x + (y * width) + (3 * width * height);
					ditherPixels[index0] = shouldDither0;
					ditherPixels[index1] = shouldDither1;
					ditherPixels[index2] = shouldDither2;
					ditherPixels[index3] = shouldDither3;
				}
			}
		}
		else
		{
			ditherBuffer.clear();
		}
	}

	void ClearFrameBuffers(BufferView2D<uint8_t> paletteIndexBuffer, BufferView2D<double> depthBuffer,
		BufferView2D<uint32_t> colorBuffer)
	{
		paletteIndexBuffer.fill(0);
		depthBuffer.fill(std::numeric_limits<double>::infinity());
		colorBuffer.fill(0);
		g_totalDepthTests = 0;
		g_totalColorWrites = 0;
	}

	void RasterizeMesh(int meshIndex, TextureSamplingType textureSamplingType0, TextureSamplingType textureSamplingType1,
		RenderLightingType lightingType, double meshLightPercent, double ambientPercent, const SoftwareRenderer::Light* const *lights,
		int lightCount, PixelShaderType pixelShaderType, double pixelShaderParam0, bool enableDepthRead, bool enableDepthWrite,
		const SoftwareRenderer::ObjectTexturePool &textures, const SoftwareRenderer::ObjectTexture &paletteTexture,
		const SoftwareRenderer::ObjectTexture &lightTableTexture, const SoftwareRenderer::ObjectTexture &skyBgTexture, int ditheringMode,
		const RenderCamera &camera, BufferView2D<uint8_t> paletteIndexBuffer, BufferView2D<double> depthBuffer, BufferView3D<const bool> ditherBuffer,
		BufferView2D<uint32_t> colorBuffer)
	{
		const int frameBufferWidth = paletteIndexBuffer.getWidth();
		const int frameBufferHeight = paletteIndexBuffer.getHeight();
		const int frameBufferPixelCount = frameBufferWidth * frameBufferHeight;
		const double frameBufferWidthReal = static_cast<double>(frameBufferWidth);
		const double frameBufferHeightReal = static_cast<double>(frameBufferHeight);
		const double frameBufferWidthRealRecip = 1.0 / frameBufferWidthReal;
		const double frameBufferHeightRealRecip = 1.0 / frameBufferHeightReal;
		const bool *ditherBufferPtr = ditherBuffer.begin();
		uint32_t *colorBufferPtr = colorBuffer.begin();

		const bool requiresTwoTextures =
			(pixelShaderType == PixelShaderType::OpaqueWithAlphaTestLayer) ||
			(pixelShaderType == PixelShaderType::AlphaTestedWithPaletteIndexLookup);
		const bool requiresHorizonMirror = pixelShaderType == PixelShaderType::AlphaTestedWithHorizonMirror;
		const bool requiresPerPixelLightIntensity = lightingType == RenderLightingType::PerPixel;
		const bool requiresPerMeshLightIntensity = lightingType == RenderLightingType::PerMesh;

		PixelShaderLighting shaderLighting;
		shaderLighting.lightTableTexels = lightTableTexture.texels8Bit;
		shaderLighting.lightLevelCount = lightTableTexture.height;
		shaderLighting.lightLevelCountReal = static_cast<double>(shaderLighting.lightLevelCount);
		shaderLighting.lastLightLevel = shaderLighting.lightLevelCount - 1;
		shaderLighting.texelsPerLightLevel = lightTableTexture.width;
		shaderLighting.lightLevel = 0;

		PixelShaderFrameBuffer shaderFrameBuffer;
		shaderFrameBuffer.colors = paletteIndexBuffer.begin();
		shaderFrameBuffer.depth = depthBuffer.begin();
		shaderFrameBuffer.palette.colors = paletteTexture.texels32Bit;
		shaderFrameBuffer.palette.count = paletteTexture.texelCount;
		shaderFrameBuffer.enableDepthWrite = enableDepthWrite;

		PixelShaderHorizonMirror shaderHorizonMirror;
		if (requiresHorizonMirror)
		{
			// @todo: this doesn't support roll. will need something like a vector projection later.
			const Double3 horizonWorldPoint = camera.worldPoint + camera.horizonDir;
			const Double4 horizonCameraPoint = RendererUtils::worldSpaceToCameraSpace(Double4(horizonWorldPoint, 1.0), g_viewMatrix);
			const Double4 horizonClipPoint = RendererUtils::cameraSpaceToClipSpace(horizonCameraPoint, g_projMatrix);
			const Double3 horizonNdcPoint = RendererUtils::clipSpaceToNDC(horizonClipPoint);
			const Double2 horizonScreenSpacePoint = RendererUtils::ndcToScreenSpace(horizonNdcPoint, frameBufferWidthReal, frameBufferHeightReal);
			shaderHorizonMirror.horizonScreenSpacePoint = horizonScreenSpacePoint;

			DebugAssert(skyBgTexture.texelCount > 0);
			shaderHorizonMirror.fallbackSkyColor = skyBgTexture.texels8Bit[0];
		}

		const auto &clipSpaceMeshV0s = g_meshProcessCaches.clipSpaceMeshV0Arrays[meshIndex];
		const auto &clipSpaceMeshV1s = g_meshProcessCaches.clipSpaceMeshV1Arrays[meshIndex];
		const auto &clipSpaceMeshV2s = g_meshProcessCaches.clipSpaceMeshV2Arrays[meshIndex];
		const auto &clipSpaceMeshUV0s = g_meshProcessCaches.clipSpaceMeshUV0Arrays[meshIndex];
		const auto &clipSpaceMeshUV1s = g_meshProcessCaches.clipSpaceMeshUV1Arrays[meshIndex];
		const auto &clipSpaceMeshUV2s = g_meshProcessCaches.clipSpaceMeshUV2Arrays[meshIndex];
		const ObjectTextureID textureID0 = g_meshProcessCaches.textureID0s[meshIndex];
		const ObjectTextureID textureID1 = g_meshProcessCaches.textureID1s[meshIndex];

		const int triangleCount = g_meshProcessCaches.clipSpaceMeshTriangleCounts[meshIndex];
		for (int triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++)
		{
			const Double4 &clip0 = clipSpaceMeshV0s[triangleIndex];
			const Double4 &clip1 = clipSpaceMeshV1s[triangleIndex];
			const Double4 &clip2 = clipSpaceMeshV2s[triangleIndex];
			const Double3 ndc0 = RendererUtils::clipSpaceToNDC(clip0);
			const Double3 ndc1 = RendererUtils::clipSpaceToNDC(clip1);
			const Double3 ndc2 = RendererUtils::clipSpaceToNDC(clip2);
			const Double2 screenSpace0 = RendererUtils::ndcToScreenSpace(ndc0, frameBufferWidthReal, frameBufferHeightReal);
			const Double2 screenSpace1 = RendererUtils::ndcToScreenSpace(ndc1, frameBufferWidthReal, frameBufferHeightReal);
			const Double2 screenSpace2 = RendererUtils::ndcToScreenSpace(ndc2, frameBufferWidthReal, frameBufferHeightReal);
			const Double2 screenSpace01 = screenSpace1 - screenSpace0;
			const Double2 screenSpace12 = screenSpace2 - screenSpace1;
			const Double2 screenSpace20 = screenSpace0 - screenSpace2;
			const double screenSpace01Cross12 = screenSpace12.cross(screenSpace01);
			const double screenSpace12Cross20 = screenSpace20.cross(screenSpace12);
			const double screenSpace20Cross01 = screenSpace01.cross(screenSpace20);

			// Discard back-facing.
			const bool isFrontFacing = (screenSpace01Cross12 + screenSpace12Cross20 + screenSpace20Cross01) > 0.0;
			if (!isFrontFacing)
			{
				continue;
			}

			const Double4 clipRecip0(1.0 / clip0.x, 1.0 / clip0.y, 1.0 / clip0.z, 1.0 / clip0.w);
			const Double4 clipRecip1(1.0 / clip1.x, 1.0 / clip1.y, 1.0 / clip1.z, 1.0 / clip1.w);
			const Double4 clipRecip2(1.0 / clip2.x, 1.0 / clip2.y, 1.0 / clip2.z, 1.0 / clip2.w);
			const double clip0XDivW = clip0.x * clipRecip0.w;
			const double clip0YDivW = clip0.y * clipRecip0.w;
			const double clip0ZDivW = clip0.z * clipRecip0.w;
			const double clip1XDivW = clip1.x * clipRecip1.w;
			const double clip1YDivW = clip1.y * clipRecip1.w;
			const double clip1ZDivW = clip1.z * clipRecip1.w;
			const double clip2XDivW = clip2.x * clipRecip2.w;
			const double clip2YDivW = clip2.y * clipRecip2.w;
			const double clip2ZDivW = clip2.z * clipRecip2.w;
			const Double2 screenSpace01Perp = screenSpace01.rightPerp();
			const Double2 screenSpace12Perp = screenSpace12.rightPerp();
			const Double2 screenSpace20Perp = screenSpace20.rightPerp();

			// Naive screen-space bounding box around triangle.
			const double xMin = std::min(screenSpace0.x, std::min(screenSpace1.x, screenSpace2.x));
			const double xMax = std::max(screenSpace0.x, std::max(screenSpace1.x, screenSpace2.x));
			const double yMin = std::min(screenSpace0.y, std::min(screenSpace1.y, screenSpace2.y));
			const double yMax = std::max(screenSpace0.y, std::max(screenSpace1.y, screenSpace2.y));
			const int xStart = RendererUtils::getLowerBoundedPixel(xMin, frameBufferWidth);
			const int xEnd = RendererUtils::getUpperBoundedPixel(xMax, frameBufferWidth);
			const int yStart = RendererUtils::getLowerBoundedPixel(yMin, frameBufferHeight);
			const int yEnd = RendererUtils::getUpperBoundedPixel(yMax, frameBufferHeight);

			const Double2 &uv0 = clipSpaceMeshUV0s[triangleIndex];
			const Double2 &uv1 = clipSpaceMeshUV1s[triangleIndex];
			const Double2 &uv2 = clipSpaceMeshUV2s[triangleIndex];
			const double uv0XDivW = uv0.x * clipRecip0.w;
			const double uv0YDivW = uv0.y * clipRecip0.w;
			const double uv1XDivW = uv1.x * clipRecip1.w;
			const double uv1YDivW = uv1.y * clipRecip1.w;
			const double uv2XDivW = uv2.x * clipRecip2.w;
			const double uv2YDivW = uv2.y * clipRecip2.w;

			const SoftwareRenderer::ObjectTexture &texture0 = textures.get(textureID0);

			PixelShaderTexture shaderTexture0;
			shaderTexture0.init(texture0.texels8Bit, texture0.width, texture0.height, textureSamplingType0);

			PixelShaderTexture shaderTexture1;
			if (requiresTwoTextures)
			{
				const SoftwareRenderer::ObjectTexture &texture1 = textures.get(textureID1);
				shaderTexture1.init(texture1.texels8Bit, texture1.width, texture1.height, textureSamplingType1);
			}

			for (int y = yStart; y < yEnd; y++)
			{
				shaderFrameBuffer.yPercent = (static_cast<double>(y) + 0.50) * frameBufferHeightRealRecip;

				for (int x = xStart; x < xEnd; x++)
				{
					shaderFrameBuffer.xPercent = (static_cast<double>(x) + 0.50) * frameBufferWidthRealRecip;
					const Double2 pixelCenter(
						shaderFrameBuffer.xPercent * frameBufferWidthReal,
						shaderFrameBuffer.yPercent * frameBufferHeightReal);

					// See if pixel center is inside triangle.
					const bool inHalfSpace0 = MathUtils::isPointInHalfSpace(pixelCenter, screenSpace0, screenSpace01Perp);
					const bool inHalfSpace1 = MathUtils::isPointInHalfSpace(pixelCenter, screenSpace1, screenSpace12Perp);
					const bool inHalfSpace2 = MathUtils::isPointInHalfSpace(pixelCenter, screenSpace2, screenSpace20Perp);
					if (inHalfSpace0 && inHalfSpace1 && inHalfSpace2)
					{
						const Double2 &ss0 = screenSpace01;
						const Double2 ss1 = screenSpace2 - screenSpace0;
						const Double2 ss2 = pixelCenter - screenSpace0;
						const double dot00 = ss0.dot(ss0);
						const double dot01 = ss0.dot(ss1);
						const double dot11 = ss1.dot(ss1);
						const double dot20 = ss2.dot(ss0);
						const double dot21 = ss2.dot(ss1);
						const double denominator = (dot00 * dot11) - (dot01 * dot01);
						const double v = ((dot11 * dot20) - (dot01 * dot21)) / denominator;
						const double w = ((dot00 * dot21) - (dot01 * dot20)) / denominator;
						const double u = 1.0 - v - w;

						PixelShaderPerspectiveCorrection shaderPerspective;
						shaderPerspective.ndcZDepth = (ndc0.z * u) + (ndc1.z * v) + (ndc2.z * w);

						shaderFrameBuffer.pixelIndex = x + (y * frameBufferWidth);
						if (enableDepthRead)
						{
							g_totalDepthTests++;
						}

						if (!enableDepthRead || (shaderPerspective.ndcZDepth < shaderFrameBuffer.depth[shaderFrameBuffer.pixelIndex]))
						{
							const Double4 shaderClipSpacePoint(
								(clip0XDivW * u) + (clip1XDivW * v) + (clip2XDivW * w),
								(clip0YDivW * u) + (clip1YDivW * v) + (clip2YDivW * w),
								(clip0ZDivW * u) + (clip1ZDivW * v) + (clip2ZDivW * w),
								(clipRecip0.w * u) + (clipRecip1.w * v) + (clipRecip2.w * w));
							const double shaderClipSpaceWRecip = 1.0 / shaderClipSpacePoint.w;

							shaderPerspective.texelPercent.x = ((uv0XDivW * u) + (uv1XDivW * v) + (uv2XDivW * w)) * shaderClipSpaceWRecip;
							shaderPerspective.texelPercent.y = ((uv0YDivW * u) + (uv1YDivW * v) + (uv2YDivW * w)) * shaderClipSpaceWRecip;

							const Double4 shaderHomogeneousSpacePoint(
								shaderClipSpacePoint.x * shaderClipSpaceWRecip,
								shaderClipSpacePoint.y * shaderClipSpaceWRecip,
								shaderClipSpacePoint.z * shaderClipSpaceWRecip,
								shaderClipSpaceWRecip);
							const Double4 shaderCameraSpacePoint = g_invProjMatrix * shaderHomogeneousSpacePoint;
							const Double4 shaderWorldSpacePoint = g_invViewMatrix * shaderCameraSpacePoint;
							const Double3 shaderWorldSpacePointXYZ(shaderWorldSpacePoint.x, shaderWorldSpacePoint.y, shaderWorldSpacePoint.z);

							double lightIntensitySum = 0.0;
							if (requiresPerPixelLightIntensity)
							{
								lightIntensitySum = ambientPercent;
								for (int lightIndex = 0; lightIndex < lightCount; lightIndex++)
								{
									const SoftwareRenderer::Light &light = *lights[lightIndex];
									const Double3 lightPointDiff = light.worldPoint - shaderWorldSpacePointXYZ;
									const double lightDistance = lightPointDiff.length();
									double lightIntensity;
									if (lightDistance <= light.startRadius)
									{
										lightIntensity = 1.0;
									}
									else if (lightDistance >= light.endRadius)
									{
										lightIntensity = 0.0;
									}
									else
									{
										const double lightDistancePercent = (lightDistance - light.startRadius) * light.startEndRadiusDiffRecip;
										lightIntensity = std::clamp(1.0 - lightDistancePercent, 0.0, 1.0);
									}

									lightIntensitySum += lightIntensity;

									if (lightIntensitySum >= 1.0)
									{
										lightIntensitySum = 1.0;
										break;
									}
								}
							}
							else if (requiresPerMeshLightIntensity)
							{
								lightIntensitySum = meshLightPercent;
							}

							const double lightLevelReal = lightIntensitySum * shaderLighting.lightLevelCountReal;
							shaderLighting.lightLevel = shaderLighting.lastLightLevel - std::clamp(static_cast<int>(lightLevelReal), 0, shaderLighting.lastLightLevel);

							if (requiresPerPixelLightIntensity)
							{
								// Dither the light level in screen space.
								bool shouldDither;
								switch (ditheringMode)
								{
								case DITHERING_MODE_NONE:
									shouldDither = false;
									break;
								case DITHERING_MODE_CLASSIC:
									shouldDither = ditherBufferPtr[shaderFrameBuffer.pixelIndex];
									break;
								case DITHERING_MODE_MODERN:
									if (lightIntensitySum < 1.0) // Keeps from dithering right next to the camera, not sure why the lowest dither level doesn't do this.
									{
										constexpr int maskCount = DITHERING_MODE_MODERN_MASK_COUNT;
										const double lightLevelFraction = lightLevelReal - std::floor(lightLevelReal);
										const int maskIndex = std::clamp(static_cast<int>(static_cast<double>(maskCount) * lightLevelFraction), 0, maskCount - 1);
										const int ditherBufferIndex = shaderFrameBuffer.pixelIndex + (maskIndex * frameBufferPixelCount);
										shouldDither = ditherBufferPtr[ditherBufferIndex];
									}
									else
									{
										shouldDither = false;
									}
									break;
								default:
									shouldDither = false;
									break;
								}

								if (shouldDither)
								{
									shaderLighting.lightLevel = std::min(shaderLighting.lightLevel + 1, shaderLighting.lastLightLevel);
								}
							}

							if (requiresHorizonMirror)
							{
								// @todo: support camera roll
								const Double2 reflectedScreenSpacePoint(
									pixelCenter.x,
									shaderHorizonMirror.horizonScreenSpacePoint.y + (shaderHorizonMirror.horizonScreenSpacePoint.y - pixelCenter.y));

								const int reflectedPixelX = static_cast<int>(reflectedScreenSpacePoint.x);
								const int reflectedPixelY = static_cast<int>(reflectedScreenSpacePoint.y);
								shaderHorizonMirror.isReflectedPixelInFrameBuffer =
									(reflectedPixelX >= 0) && (reflectedPixelX < frameBufferWidth) &&
									(reflectedPixelY >= 0) && (reflectedPixelY < frameBufferHeight);
								shaderHorizonMirror.reflectedPixelIndex = reflectedPixelX + (reflectedPixelY * frameBufferWidth);
							}

							switch (pixelShaderType)
							{
							case PixelShaderType::Opaque:
								PixelShader_Opaque(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::OpaqueWithAlphaTestLayer:
								PixelShader_OpaqueWithAlphaTestLayer(shaderPerspective, shaderTexture0, shaderTexture1, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTested:
								PixelShader_AlphaTested(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithVariableTexCoordUMin:
								PixelShader_AlphaTestedWithVariableTexCoordUMin(shaderPerspective, shaderTexture0, pixelShaderParam0, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithVariableTexCoordVMin:
								PixelShader_AlphaTestedWithVariableTexCoordVMin(shaderPerspective, shaderTexture0, pixelShaderParam0, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithPaletteIndexLookup:
								PixelShader_AlphaTestedWithPaletteIndexLookup(shaderPerspective, shaderTexture0, shaderTexture1, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithLightLevelColor:
								PixelShader_AlphaTestedWithLightLevelColor(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithLightLevelOpacity:
								PixelShader_AlphaTestedWithLightLevelOpacity(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithPreviousBrightnessLimit:
								PixelShader_AlphaTestedWithPreviousBrightnessLimit(shaderPerspective, shaderTexture0, shaderFrameBuffer);
								break;
							case PixelShaderType::AlphaTestedWithHorizonMirror:
								PixelShader_AlphaTestedWithHorizonMirror(shaderPerspective, shaderTexture0, shaderHorizonMirror, shaderLighting, shaderFrameBuffer);
								break;
							default:
								DebugNotImplementedMsg(std::to_string(static_cast<int>(pixelShaderType)));
								break;
							}

							// Write pixel shader result to final output buffer. This only results in overdraw for ghosts.
							const uint8_t writtenPaletteIndex = shaderFrameBuffer.colors[shaderFrameBuffer.pixelIndex];
							colorBufferPtr[shaderFrameBuffer.pixelIndex] = shaderFrameBuffer.palette.colors[writtenPaletteIndex];
							g_totalColorWrites++;
						}
					}
				}
			}
		}
	}
}

SoftwareRenderer::ObjectTexture::ObjectTexture()
{
	this->texels8Bit = nullptr;
	this->texels32Bit = nullptr;
	this->width = 0;
	this->height = 0;
	this->widthReal = 0.0;
	this->heightReal = 0.0;
	this->texelCount = 0;
	this->bytesPerTexel = 0;
}

void SoftwareRenderer::ObjectTexture::init(int width, int height, int bytesPerTexel)
{
	DebugAssert(width > 0);
	DebugAssert(height > 0);
	DebugAssert(bytesPerTexel > 0);

	this->texelCount = width * height;
	this->texels.init(this->texelCount * bytesPerTexel);
	this->texels.fill(static_cast<std::byte>(0));

	switch (bytesPerTexel)
	{
	case 1:
		this->texels8Bit = reinterpret_cast<const uint8_t*>(this->texels.begin());
		break;
	case 4:
		this->texels32Bit = reinterpret_cast<const uint32_t*>(this->texels.begin());
		break;
	default:
		DebugNotImplementedMsg(std::to_string(bytesPerTexel));
		break;
	}

	this->width = width;
	this->height = height;
	this->widthReal = static_cast<double>(width);
	this->heightReal = static_cast<double>(height);
	this->bytesPerTexel = bytesPerTexel;
}

void SoftwareRenderer::ObjectTexture::clear()
{
	this->texels.clear();
}

void SoftwareRenderer::VertexBuffer::init(int vertexCount, int componentsPerVertex)
{
	const int valueCount = vertexCount * componentsPerVertex;
	this->vertices.init(valueCount);
}

void SoftwareRenderer::AttributeBuffer::init(int vertexCount, int componentsPerVertex)
{
	const int valueCount = vertexCount * componentsPerVertex;
	this->attributes.init(valueCount);
}

void SoftwareRenderer::IndexBuffer::init(int indexCount)
{
	DebugAssertMsg((indexCount % 3) == 0, "Expected index buffer to have multiple of 3 indices (has " + std::to_string(indexCount) + ").");
	this->indices.init(indexCount);
	this->triangleCount = indexCount / 3;
}

SoftwareRenderer::Light::Light()
{
	this->startRadius = 0.0;
	this->endRadius = 0.0;
	this->startEndRadiusDiff = 0.0;
	this->startEndRadiusDiffRecip = 0.0;
}

void SoftwareRenderer::Light::init(const Double3 &worldPoint, double startRadius, double endRadius)
{
	this->worldPoint = worldPoint;
	this->startRadius = startRadius;
	this->endRadius = endRadius;
	this->startEndRadiusDiff = endRadius - startRadius;
	this->startEndRadiusDiffRecip = 1.0 / this->startEndRadiusDiff;
}

SoftwareRenderer::SoftwareRenderer()
{
	this->ditheringMode = -1;
}

SoftwareRenderer::~SoftwareRenderer()
{

}

void SoftwareRenderer::init(const RenderInitSettings &settings)
{
	this->paletteIndexBuffer.init(settings.width, settings.height);
	this->depthBuffer.init(settings.width, settings.height);

	CreateDitherBuffer(this->ditherBuffer, settings.width, settings.height, settings.ditheringMode);
	this->ditheringMode = settings.ditheringMode;
}

void SoftwareRenderer::shutdown()
{
	this->paletteIndexBuffer.clear();
	this->depthBuffer.clear();
	this->ditherBuffer.clear();
	this->ditheringMode = -1;
	this->vertexBuffers.clear();
	this->attributeBuffers.clear();
	this->indexBuffers.clear();
	this->uniformBuffers.clear();
	this->objectTextures.clear();
	this->lights.clear();
}

bool SoftwareRenderer::isInited() const
{
	return true;
}

void SoftwareRenderer::resize(int width, int height)
{
	this->paletteIndexBuffer.init(width, height);
	this->paletteIndexBuffer.fill(0);

	this->depthBuffer.init(width, height);
	this->depthBuffer.fill(std::numeric_limits<double>::infinity());

	CreateDitherBuffer(this->ditherBuffer, width, height, this->ditheringMode);
}

bool SoftwareRenderer::tryCreateVertexBuffer(int vertexCount, int componentsPerVertex, VertexBufferID *outID)
{
	DebugAssert(vertexCount > 0);
	DebugAssert(componentsPerVertex >= 2);

	if (!this->vertexBuffers.tryAlloc(outID))
	{
		DebugLogError("Couldn't allocate vertex buffer ID.");
		return false;
	}

	VertexBuffer &buffer = this->vertexBuffers.get(*outID);
	buffer.init(vertexCount, componentsPerVertex);
	return true;
}

bool SoftwareRenderer::tryCreateAttributeBuffer(int vertexCount, int componentsPerVertex, AttributeBufferID *outID)
{
	DebugAssert(vertexCount > 0);
	DebugAssert(componentsPerVertex >= 2);

	if (!this->attributeBuffers.tryAlloc(outID))
	{
		DebugLogError("Couldn't allocate attribute buffer ID.");
		return false;
	}

	AttributeBuffer &buffer = this->attributeBuffers.get(*outID);
	buffer.init(vertexCount, componentsPerVertex);
	return true;
}

bool SoftwareRenderer::tryCreateIndexBuffer(int indexCount, IndexBufferID *outID)
{
	DebugAssert(indexCount > 0);
	DebugAssert((indexCount % 3) == 0);

	if (!this->indexBuffers.tryAlloc(outID))
	{
		DebugLogError("Couldn't allocate index buffer ID.");
		return false;
	}

	IndexBuffer &buffer = this->indexBuffers.get(*outID);
	buffer.init(indexCount);
	return true;
}

void SoftwareRenderer::populateVertexBuffer(VertexBufferID id, BufferView<const double> vertices)
{
	VertexBuffer &buffer = this->vertexBuffers.get(id);
	const int srcCount = vertices.getCount();
	const int dstCount = buffer.vertices.getCount();
	if (srcCount != dstCount)
	{
		DebugLogError("Mismatched vertex buffer sizes for ID " + std::to_string(id) + ": " +
			std::to_string(srcCount) + " != " + std::to_string(dstCount));
		return;
	}

	const auto srcBegin = vertices.begin();
	const auto srcEnd = srcBegin + srcCount;
	std::copy(srcBegin, srcEnd, buffer.vertices.begin());
}

void SoftwareRenderer::populateAttributeBuffer(AttributeBufferID id, BufferView<const double> attributes)
{
	AttributeBuffer &buffer = this->attributeBuffers.get(id);
	const int srcCount = attributes.getCount();
	const int dstCount = buffer.attributes.getCount();
	if (srcCount != dstCount)
	{
		DebugLogError("Mismatched attribute buffer sizes for ID " + std::to_string(id) + ": " +
			std::to_string(srcCount) + " != " + std::to_string(dstCount));
		return;
	}

	const auto srcBegin = attributes.begin();
	const auto srcEnd = srcBegin + srcCount;
	std::copy(srcBegin, srcEnd, buffer.attributes.begin());
}

void SoftwareRenderer::populateIndexBuffer(IndexBufferID id, BufferView<const int32_t> indices)
{
	IndexBuffer &buffer = this->indexBuffers.get(id);
	const int srcCount = indices.getCount();
	const int dstCount = buffer.indices.getCount();
	if (srcCount != dstCount)
	{
		DebugLogError("Mismatched index buffer sizes for ID " + std::to_string(id) + ": " +
			std::to_string(srcCount) + " != " + std::to_string(dstCount));
		return;
	}

	const auto srcBegin = indices.begin();
	const auto srcEnd = srcBegin + srcCount;
	std::copy(srcBegin, srcEnd, buffer.indices.begin());
}

void SoftwareRenderer::freeVertexBuffer(VertexBufferID id)
{
	this->vertexBuffers.free(id);
}

void SoftwareRenderer::freeAttributeBuffer(AttributeBufferID id)
{
	this->attributeBuffers.free(id);
}

void SoftwareRenderer::freeIndexBuffer(IndexBufferID id)
{
	this->indexBuffers.free(id);
}

bool SoftwareRenderer::tryCreateObjectTexture(int width, int height, int bytesPerTexel, ObjectTextureID *outID)
{
	if (!this->objectTextures.tryAlloc(outID))
	{
		DebugLogError("Couldn't allocate object texture ID.");
		return false;
	}

	ObjectTexture &texture = this->objectTextures.get(*outID);
	texture.init(width, height, bytesPerTexel);
	return true;
}

bool SoftwareRenderer::tryCreateObjectTexture(const TextureBuilder &textureBuilder, ObjectTextureID *outID)
{
	const int width = textureBuilder.getWidth();
	const int height = textureBuilder.getHeight();
	const int bytesPerTexel = textureBuilder.getBytesPerTexel();
	if (!this->tryCreateObjectTexture(width, height, bytesPerTexel, outID))
	{
		DebugLogWarning("Couldn't create " + std::to_string(width) + "x" + std::to_string(height) + " object texture.");
		return false;
	}

	const TextureBuilderType textureBuilderType = textureBuilder.getType();
	ObjectTexture &texture = this->objectTextures.get(*outID);
	if (textureBuilderType == TextureBuilderType::Paletted)
	{
		const TextureBuilder::PalettedTexture &palettedTexture = textureBuilder.getPaletted();
		const Buffer2D<uint8_t> &srcTexels = palettedTexture.texels;
		uint8_t *dstTexels = reinterpret_cast<uint8_t*>(texture.texels.begin());
		std::copy(srcTexels.begin(), srcTexels.end(), dstTexels);
	}
	else if (textureBuilderType == TextureBuilderType::TrueColor)
	{
		const TextureBuilder::TrueColorTexture &trueColorTexture = textureBuilder.getTrueColor();
		const Buffer2D<uint32_t> &srcTexels = trueColorTexture.texels;
		uint32_t *dstTexels = reinterpret_cast<uint32_t*>(texture.texels.begin());
		std::copy(srcTexels.begin(), srcTexels.end(), dstTexels);
	}
	else
	{
		DebugUnhandledReturnMsg(bool, std::to_string(static_cast<int>(textureBuilderType)));
	}

	return true;
}

LockedTexture SoftwareRenderer::lockObjectTexture(ObjectTextureID id)
{
	ObjectTexture &texture = this->objectTextures.get(id);
	return LockedTexture(texture.texels.begin(), texture.bytesPerTexel);
}

void SoftwareRenderer::unlockObjectTexture(ObjectTextureID id)
{
	// Do nothing; any writes are already in RAM.
	static_cast<void>(id);
}

void SoftwareRenderer::freeObjectTexture(ObjectTextureID id)
{
	this->objectTextures.free(id);
}

std::optional<Int2> SoftwareRenderer::tryGetObjectTextureDims(ObjectTextureID id) const
{
	const ObjectTexture &texture = this->objectTextures.get(id);
	return Int2(texture.width, texture.height);
}

bool SoftwareRenderer::tryCreateUniformBuffer(int elementCount, size_t sizeOfElement, size_t alignmentOfElement, UniformBufferID *outID)
{
	DebugAssert(elementCount >= 0);
	DebugAssert(sizeOfElement > 0);
	DebugAssert(alignmentOfElement > 0);

	if (!this->uniformBuffers.tryAlloc(outID))
	{
		DebugLogError("Couldn't allocate uniform buffer ID.");
		return false;
	}

	UniformBuffer &buffer = this->uniformBuffers.get(*outID);
	buffer.init(elementCount, sizeOfElement, alignmentOfElement);
	return true;
}

void SoftwareRenderer::populateUniformBuffer(UniformBufferID id, BufferView<const std::byte> data)
{
	UniformBuffer &buffer = this->uniformBuffers.get(id);
	const int srcCount = data.getCount();
	const int dstCount = buffer.getValidByteCount();
	if (srcCount != dstCount)
	{
		DebugLogError("Mismatched uniform buffer sizes for ID " + std::to_string(id) + ": " +
			std::to_string(srcCount) + " != " + std::to_string(dstCount));
		return;
	}

	const std::byte *srcBegin = data.begin();
	const std::byte *srcEnd = srcBegin + srcCount;
	std::copy(srcBegin, srcEnd, buffer.begin());
}

void SoftwareRenderer::populateUniformAtIndex(UniformBufferID id, int uniformIndex, BufferView<const std::byte> uniformData)
{
	UniformBuffer &buffer = this->uniformBuffers.get(id);
	const int srcByteCount = uniformData.getCount();
	const int dstByteCount = static_cast<int>(buffer.sizeOfElement);
	if (srcByteCount != dstByteCount)
	{
		DebugLogError("Mismatched uniform size for uniform buffer ID " + std::to_string(id) + " index " +
			std::to_string(uniformIndex) + ": " + std::to_string(srcByteCount) + " != " + std::to_string(dstByteCount));
		return;
	}

	const std::byte *srcBegin = uniformData.begin();
	const std::byte *srcEnd = srcBegin + srcByteCount;
	std::byte *dstBegin = buffer.begin() + (dstByteCount * uniformIndex);
	std::copy(srcBegin, srcEnd, dstBegin);
}

void SoftwareRenderer::freeUniformBuffer(UniformBufferID id)
{
	this->uniformBuffers.free(id);
}

bool SoftwareRenderer::tryCreateLight(RenderLightID *outID)
{
	if (!this->lights.tryAlloc(outID))
	{
		DebugLogError("Couldn't allocate render light ID.");
		return false;
	}

	return true;
}

void SoftwareRenderer::setLightPosition(RenderLightID id, const Double3 &worldPoint)
{
	Light &light = this->lights.get(id);
	light.worldPoint = worldPoint;
}

void SoftwareRenderer::setLightRadius(RenderLightID id, double startRadius, double endRadius)
{
	DebugAssert(startRadius >= 0.0);
	DebugAssert(endRadius >= startRadius);
	Light &light = this->lights.get(id);
	light.startRadius = startRadius;
	light.endRadius = endRadius;
	light.startEndRadiusDiff = endRadius - startRadius;
	light.startEndRadiusDiffRecip = 1.0 / light.startEndRadiusDiff;
}

void SoftwareRenderer::freeLight(RenderLightID id)
{
	this->lights.free(id);
}

RendererSystem3D::ProfilerData SoftwareRenderer::getProfilerData() const
{
	const int renderWidth = this->paletteIndexBuffer.getWidth();
	const int renderHeight = this->paletteIndexBuffer.getHeight();

	const int threadCount = 1;

	const int drawCallCount = g_totalDrawCallCount;
	const int sceneTriangleCount = g_totalDrawCallTriangleCount;
	const int visTriangleCount = g_totalClipSpaceTriangleCount;

	const int textureCount = this->objectTextures.getUsedCount();
	int textureByteCount = 0;
	for (int i = 0; i < this->objectTextures.getTotalCount(); i++)
	{
		const ObjectTextureID id = static_cast<ObjectTextureID>(i);
		const ObjectTexture *texturePtr = this->objectTextures.tryGet(id);
		if (texturePtr != nullptr)
		{
			textureByteCount += texturePtr->texels.getCount();
		}
	}

	const int totalLightCount = this->lights.getUsedCount();
	const int totalDepthTests = g_totalDepthTests;
	const int totalColorWrites = g_totalColorWrites;

	return ProfilerData(renderWidth, renderHeight, threadCount, drawCallCount, sceneTriangleCount,
		visTriangleCount, textureCount, textureByteCount, totalLightCount, totalDepthTests, totalColorWrites);
}

void SoftwareRenderer::submitFrame(const RenderCamera &camera, BufferView<const RenderDrawCall> drawCalls,
	const RenderFrameSettings &settings, uint32_t *outputBuffer)
{
	const int frameBufferWidth = this->paletteIndexBuffer.getWidth();
	const int frameBufferHeight = this->paletteIndexBuffer.getHeight();

	const double ambientPercent = settings.ambientPercent;
	if (this->ditheringMode != settings.ditheringMode)
	{
		this->ditheringMode = settings.ditheringMode;
		CreateDitherBuffer(this->ditherBuffer, frameBufferWidth, frameBufferHeight, settings.ditheringMode);
	}

	BufferView2D<uint8_t> paletteIndexBufferView(this->paletteIndexBuffer.begin(), frameBufferWidth, frameBufferHeight);
	BufferView2D<double> depthBufferView(this->depthBuffer.begin(), frameBufferWidth, frameBufferHeight);
	BufferView3D<const bool> ditherBufferView(this->ditherBuffer.begin(), frameBufferWidth, frameBufferHeight, this->ditherBuffer.getDepth());
	BufferView2D<uint32_t> colorBufferView(outputBuffer, frameBufferWidth, frameBufferHeight);

	// Palette for 8-bit -> 32-bit color conversion.
	const ObjectTexture &paletteTexture = this->objectTextures.get(settings.paletteTextureID);

	// Light table for shading/transparency look-ups.
	const ObjectTexture &lightTableTexture = this->objectTextures.get(settings.lightTableTextureID);

	// Sky texture for horizon reflection shader.
	const ObjectTexture &skyBgTexture = this->objectTextures.get(settings.skyBgTextureID);

	ClearFrameBuffers(paletteIndexBufferView, depthBufferView, colorBufferView);
	ClearTriangleTotalCounts();
	PopulateCameraGlobals(camera);

	const RenderDrawCall *drawCallsPtr = drawCalls.begin();
	const int drawCallCount = drawCalls.getCount();
	g_totalDrawCallCount = drawCallCount;

	auto &meshProcessCacheTranslationMatrixXXs = g_meshProcessCaches.translationMatrixXXs;
	auto &meshProcessCacheTranslationMatrixXYs = g_meshProcessCaches.translationMatrixXYs;
	auto &meshProcessCacheTranslationMatrixXZs = g_meshProcessCaches.translationMatrixXZs;
	auto &meshProcessCacheTranslationMatrixXWs = g_meshProcessCaches.translationMatrixXWs;
	auto &meshProcessCacheTranslationMatrixYXs = g_meshProcessCaches.translationMatrixYXs;
	auto &meshProcessCacheTranslationMatrixYYs = g_meshProcessCaches.translationMatrixYYs;
	auto &meshProcessCacheTranslationMatrixYZs = g_meshProcessCaches.translationMatrixYZs;
	auto &meshProcessCacheTranslationMatrixYWs = g_meshProcessCaches.translationMatrixYWs;
	auto &meshProcessCacheTranslationMatrixZXs = g_meshProcessCaches.translationMatrixZXs;
	auto &meshProcessCacheTranslationMatrixZYs = g_meshProcessCaches.translationMatrixZYs;
	auto &meshProcessCacheTranslationMatrixZZs = g_meshProcessCaches.translationMatrixZZs;
	auto &meshProcessCacheTranslationMatrixZWs = g_meshProcessCaches.translationMatrixZWs;
	auto &meshProcessCacheTranslationMatrixWXs = g_meshProcessCaches.translationMatrixWXs;
	auto &meshProcessCacheTranslationMatrixWYs = g_meshProcessCaches.translationMatrixWYs;
	auto &meshProcessCacheTranslationMatrixWZs = g_meshProcessCaches.translationMatrixWZs;
	auto &meshProcessCacheTranslationMatrixWWs = g_meshProcessCaches.translationMatrixWWs;
	auto &meshProcessCacheRotationMatrixXXs = g_meshProcessCaches.rotationMatrixXXs;
	auto &meshProcessCacheRotationMatrixXYs = g_meshProcessCaches.rotationMatrixXYs;
	auto &meshProcessCacheRotationMatrixXZs = g_meshProcessCaches.rotationMatrixXZs;
	auto &meshProcessCacheRotationMatrixXWs = g_meshProcessCaches.rotationMatrixXWs;
	auto &meshProcessCacheRotationMatrixYXs = g_meshProcessCaches.rotationMatrixYXs;
	auto &meshProcessCacheRotationMatrixYYs = g_meshProcessCaches.rotationMatrixYYs;
	auto &meshProcessCacheRotationMatrixYZs = g_meshProcessCaches.rotationMatrixYZs;
	auto &meshProcessCacheRotationMatrixYWs = g_meshProcessCaches.rotationMatrixYWs;
	auto &meshProcessCacheRotationMatrixZXs = g_meshProcessCaches.rotationMatrixZXs;
	auto &meshProcessCacheRotationMatrixZYs = g_meshProcessCaches.rotationMatrixZYs;
	auto &meshProcessCacheRotationMatrixZZs = g_meshProcessCaches.rotationMatrixZZs;
	auto &meshProcessCacheRotationMatrixZWs = g_meshProcessCaches.rotationMatrixZWs;
	auto &meshProcessCacheRotationMatrixWXs = g_meshProcessCaches.rotationMatrixWXs;
	auto &meshProcessCacheRotationMatrixWYs = g_meshProcessCaches.rotationMatrixWYs;
	auto &meshProcessCacheRotationMatrixWZs = g_meshProcessCaches.rotationMatrixWZs;
	auto &meshProcessCacheRotationMatrixWWs = g_meshProcessCaches.rotationMatrixWWs;
	auto &meshProcessCacheScaleMatrixXXs = g_meshProcessCaches.scaleMatrixXXs;
	auto &meshProcessCacheScaleMatrixXYs = g_meshProcessCaches.scaleMatrixXYs;
	auto &meshProcessCacheScaleMatrixXZs = g_meshProcessCaches.scaleMatrixXZs;
	auto &meshProcessCacheScaleMatrixXWs = g_meshProcessCaches.scaleMatrixXWs;
	auto &meshProcessCacheScaleMatrixYXs = g_meshProcessCaches.scaleMatrixYXs;
	auto &meshProcessCacheScaleMatrixYYs = g_meshProcessCaches.scaleMatrixYYs;
	auto &meshProcessCacheScaleMatrixYZs = g_meshProcessCaches.scaleMatrixYZs;
	auto &meshProcessCacheScaleMatrixYWs = g_meshProcessCaches.scaleMatrixYWs;
	auto &meshProcessCacheScaleMatrixZXs = g_meshProcessCaches.scaleMatrixZXs;
	auto &meshProcessCacheScaleMatrixZYs = g_meshProcessCaches.scaleMatrixZYs;
	auto &meshProcessCacheScaleMatrixZZs = g_meshProcessCaches.scaleMatrixZZs;
	auto &meshProcessCacheScaleMatrixZWs = g_meshProcessCaches.scaleMatrixZWs;
	auto &meshProcessCacheScaleMatrixWXs = g_meshProcessCaches.scaleMatrixWXs;
	auto &meshProcessCacheScaleMatrixWYs = g_meshProcessCaches.scaleMatrixWYs;
	auto &meshProcessCacheScaleMatrixWZs = g_meshProcessCaches.scaleMatrixWZs;
	auto &meshProcessCacheScaleMatrixWWs = g_meshProcessCaches.scaleMatrixWWs;
	auto &meshProcessCachePreScaleTranslationXs = g_meshProcessCaches.preScaleTranslationXs;
	auto &meshProcessCachePreScaleTranslationYs = g_meshProcessCaches.preScaleTranslationYs;
	auto &meshProcessCachePreScaleTranslationZs = g_meshProcessCaches.preScaleTranslationZs;
	auto &meshProcessCacheVertexBuffers = g_meshProcessCaches.vertexBuffers;
	auto &meshProcessCacheTexCoordBuffers = g_meshProcessCaches.texCoordBuffers;
	auto &meshProcessCacheIndexBuffers = g_meshProcessCaches.indexBuffers;
	auto &meshProcessCacheTextureID0s = g_meshProcessCaches.textureID0s;
	auto &meshProcessCacheTextureID1s = g_meshProcessCaches.textureID1s;
	auto &meshProcessCacheTextureSamplingType0s = g_meshProcessCaches.textureSamplingType0s;
	auto &meshProcessCacheTextureSamplingType1s = g_meshProcessCaches.textureSamplingType1s;
	auto &meshProcessCacheLightingTypes = g_meshProcessCaches.lightingTypes;
	auto &meshProcessCacheMeshLightPercents = g_meshProcessCaches.meshLightPercents;
	auto &meshProcessCacheLightPtrArrays = g_meshProcessCaches.lightPtrArrays;
	auto &meshProcessCacheLightCounts = g_meshProcessCaches.lightCounts;
	auto &meshProcessCachePixelShaderTypes = g_meshProcessCaches.pixelShaderTypes;
	auto &meshProcessCachePixelShaderParam0s = g_meshProcessCaches.pixelShaderParam0s;
	auto &meshProcessCacheEnableDepthReads = g_meshProcessCaches.enableDepthReads;
	auto &meshProcessCacheEnableDepthWrites = g_meshProcessCaches.enableDepthWrites;

	int drawCallIndex = 0;
	while (drawCallIndex < drawCallCount)
	{
		// See how many draw calls in a row can be processed with the same vertex shader.
		VertexShaderType vertexShaderType = static_cast<VertexShaderType>(-1);
		const int maxDrawCallSequenceCount = std::min(MAX_MESH_PROCESS_CACHES, drawCallCount - drawCallIndex);
		int drawCallSequenceCount = 0;
		for (int sequenceIndex = 0; sequenceIndex < maxDrawCallSequenceCount; sequenceIndex++)
		{
			const int sequenceDrawCallIndex = drawCallIndex + sequenceIndex;
			const RenderDrawCall &drawCall = drawCallsPtr[sequenceDrawCallIndex];

			const bool isBootstrap = sequenceIndex == 0;
			if (isBootstrap)
			{
				vertexShaderType = drawCall.vertexShaderType;
			}
			else if (drawCall.vertexShaderType != vertexShaderType)
			{
				break;
			}

			const UniformBuffer &transformBuffer = this->uniformBuffers.get(drawCall.transformBufferID);
			const RenderTransform &transform = transformBuffer.get<RenderTransform>(drawCall.transformIndex);
			meshProcessCacheTranslationMatrixXXs[sequenceIndex] = transform.translation.x.x;
			meshProcessCacheTranslationMatrixXYs[sequenceIndex] = transform.translation.x.y;
			meshProcessCacheTranslationMatrixXZs[sequenceIndex] = transform.translation.x.z;
			meshProcessCacheTranslationMatrixXWs[sequenceIndex] = transform.translation.x.w;
			meshProcessCacheTranslationMatrixYXs[sequenceIndex] = transform.translation.y.x;
			meshProcessCacheTranslationMatrixYYs[sequenceIndex] = transform.translation.y.y;
			meshProcessCacheTranslationMatrixYZs[sequenceIndex] = transform.translation.y.z;
			meshProcessCacheTranslationMatrixYWs[sequenceIndex] = transform.translation.y.w;
			meshProcessCacheTranslationMatrixZXs[sequenceIndex] = transform.translation.z.x;
			meshProcessCacheTranslationMatrixZYs[sequenceIndex] = transform.translation.z.y;
			meshProcessCacheTranslationMatrixZZs[sequenceIndex] = transform.translation.z.z;
			meshProcessCacheTranslationMatrixZWs[sequenceIndex] = transform.translation.z.w;
			meshProcessCacheTranslationMatrixWXs[sequenceIndex] = transform.translation.w.x;
			meshProcessCacheTranslationMatrixWYs[sequenceIndex] = transform.translation.w.y;
			meshProcessCacheTranslationMatrixWZs[sequenceIndex] = transform.translation.w.z;
			meshProcessCacheTranslationMatrixWWs[sequenceIndex] = transform.translation.w.w;
			meshProcessCacheRotationMatrixXXs[sequenceIndex] = transform.rotation.x.x;
			meshProcessCacheRotationMatrixXYs[sequenceIndex] = transform.rotation.x.y;
			meshProcessCacheRotationMatrixXZs[sequenceIndex] = transform.rotation.x.z;
			meshProcessCacheRotationMatrixXWs[sequenceIndex] = transform.rotation.x.w;
			meshProcessCacheRotationMatrixYXs[sequenceIndex] = transform.rotation.y.x;
			meshProcessCacheRotationMatrixYYs[sequenceIndex] = transform.rotation.y.y;
			meshProcessCacheRotationMatrixYZs[sequenceIndex] = transform.rotation.y.z;
			meshProcessCacheRotationMatrixYWs[sequenceIndex] = transform.rotation.y.w;
			meshProcessCacheRotationMatrixZXs[sequenceIndex] = transform.rotation.z.x;
			meshProcessCacheRotationMatrixZYs[sequenceIndex] = transform.rotation.z.y;
			meshProcessCacheRotationMatrixZZs[sequenceIndex] = transform.rotation.z.z;
			meshProcessCacheRotationMatrixZWs[sequenceIndex] = transform.rotation.z.w;
			meshProcessCacheRotationMatrixWXs[sequenceIndex] = transform.rotation.w.x;
			meshProcessCacheRotationMatrixWYs[sequenceIndex] = transform.rotation.w.y;
			meshProcessCacheRotationMatrixWZs[sequenceIndex] = transform.rotation.w.z;
			meshProcessCacheRotationMatrixWWs[sequenceIndex] = transform.rotation.w.w;
			meshProcessCacheScaleMatrixXXs[sequenceIndex] = transform.scale.x.x;
			meshProcessCacheScaleMatrixXYs[sequenceIndex] = transform.scale.x.y;
			meshProcessCacheScaleMatrixXZs[sequenceIndex] = transform.scale.x.z;
			meshProcessCacheScaleMatrixXWs[sequenceIndex] = transform.scale.x.w;
			meshProcessCacheScaleMatrixYXs[sequenceIndex] = transform.scale.y.x;
			meshProcessCacheScaleMatrixYYs[sequenceIndex] = transform.scale.y.y;
			meshProcessCacheScaleMatrixYZs[sequenceIndex] = transform.scale.y.z;
			meshProcessCacheScaleMatrixYWs[sequenceIndex] = transform.scale.y.w;
			meshProcessCacheScaleMatrixZXs[sequenceIndex] = transform.scale.z.x;
			meshProcessCacheScaleMatrixZYs[sequenceIndex] = transform.scale.z.y;
			meshProcessCacheScaleMatrixZZs[sequenceIndex] = transform.scale.z.z;
			meshProcessCacheScaleMatrixZWs[sequenceIndex] = transform.scale.z.w;
			meshProcessCacheScaleMatrixWXs[sequenceIndex] = transform.scale.w.x;
			meshProcessCacheScaleMatrixWYs[sequenceIndex] = transform.scale.w.y;
			meshProcessCacheScaleMatrixWZs[sequenceIndex] = transform.scale.w.z;
			meshProcessCacheScaleMatrixWWs[sequenceIndex] = transform.scale.w.w;
			// Do model-view-projection matrix in the bulk processing loop.

			meshProcessCachePreScaleTranslationXs[sequenceIndex] = 0.0;
			meshProcessCachePreScaleTranslationYs[sequenceIndex] = 0.0;
			meshProcessCachePreScaleTranslationZs[sequenceIndex] = 0.0;
			if (drawCall.preScaleTranslationBufferID >= 0)
			{
				const UniformBuffer &preScaleTranslationBuffer = this->uniformBuffers.get(drawCall.preScaleTranslationBufferID);
				const Double3 &preScaleTranslation = preScaleTranslationBuffer.get<Double3>(0);
				meshProcessCachePreScaleTranslationXs[sequenceIndex] = preScaleTranslation.x;
				meshProcessCachePreScaleTranslationYs[sequenceIndex] = preScaleTranslation.y;
				meshProcessCachePreScaleTranslationZs[sequenceIndex] = preScaleTranslation.z;
			}

			meshProcessCacheVertexBuffers[sequenceIndex] = &this->vertexBuffers.get(drawCall.vertexBufferID);
			meshProcessCacheTexCoordBuffers[sequenceIndex] = &this->attributeBuffers.get(drawCall.texCoordBufferID);
			meshProcessCacheIndexBuffers[sequenceIndex] = &this->indexBuffers.get(drawCall.indexBufferID);

			const ObjectTextureID *varyingTexture0 = drawCall.varyingTextures[0];
			const ObjectTextureID *varyingTexture1 = drawCall.varyingTextures[1];
			meshProcessCacheTextureID0s[sequenceIndex] = (varyingTexture0 != nullptr) ? *varyingTexture0 : drawCall.textureIDs[0];
			meshProcessCacheTextureID1s[sequenceIndex] = (varyingTexture1 != nullptr) ? *varyingTexture1 : drawCall.textureIDs[1];
			meshProcessCacheTextureSamplingType0s[sequenceIndex] = drawCall.textureSamplingTypes[0];
			meshProcessCacheTextureSamplingType1s[sequenceIndex] = drawCall.textureSamplingTypes[1];
			meshProcessCacheLightingTypes[sequenceIndex] = drawCall.lightingType;
			meshProcessCacheMeshLightPercents[sequenceIndex] = drawCall.lightPercent;

			auto &meshProcessCacheLightPtrs = meshProcessCacheLightPtrArrays[sequenceIndex];
			for (int lightIndex = 0; lightIndex < drawCall.lightIdCount; lightIndex++)
			{
				const RenderLightID lightID = drawCall.lightIDs[lightIndex];
				meshProcessCacheLightPtrs[lightIndex] = &this->lights.get(lightID);
			}

			meshProcessCacheLightCounts[sequenceIndex] = drawCall.lightIdCount;
			meshProcessCachePixelShaderTypes[sequenceIndex] = drawCall.pixelShaderType;
			meshProcessCachePixelShaderParam0s[sequenceIndex] = drawCall.pixelShaderParam0;
			meshProcessCacheEnableDepthReads[sequenceIndex] = drawCall.enableDepthRead;
			meshProcessCacheEnableDepthWrites[sequenceIndex] = drawCall.enableDepthWrite;

			drawCallSequenceCount++;
		}

		ProcessMeshBufferLookups(drawCallSequenceCount);
		CalculateVertexShaderTransforms(drawCallSequenceCount);
		ProcessVertexShaders(drawCallSequenceCount, vertexShaderType);
		ProcessClipping(drawCallSequenceCount);

		for (int meshIndex = 0; meshIndex < drawCallSequenceCount; meshIndex++)
		{
			const TextureSamplingType textureSamplingType0 = meshProcessCacheTextureSamplingType0s[meshIndex];
			const TextureSamplingType textureSamplingType1 = meshProcessCacheTextureSamplingType1s[meshIndex];
			const RenderLightingType lightingType = meshProcessCacheLightingTypes[meshIndex];
			const double meshLightPercent = meshProcessCacheMeshLightPercents[meshIndex];
			const auto &lightPtrs = meshProcessCacheLightPtrArrays[meshIndex];
			const int lightCount = meshProcessCacheLightCounts[meshIndex];
			const PixelShaderType pixelShaderType = meshProcessCachePixelShaderTypes[meshIndex];
			const double pixelShaderParam0 = meshProcessCachePixelShaderParam0s[meshIndex];
			const bool enableDepthRead = meshProcessCacheEnableDepthReads[meshIndex];
			const bool enableDepthWrite = meshProcessCacheEnableDepthWrites[meshIndex];
			RasterizeMesh(meshIndex, textureSamplingType0, textureSamplingType1, lightingType, meshLightPercent,
				ambientPercent, lightPtrs, lightCount, pixelShaderType, pixelShaderParam0, enableDepthRead, enableDepthWrite,
				this->objectTextures, paletteTexture, lightTableTexture, skyBgTexture, this->ditheringMode, camera,
				paletteIndexBufferView, depthBufferView, ditherBufferView, colorBufferView);
		}

		drawCallIndex += drawCallSequenceCount;
	}
}

void SoftwareRenderer::present()
{
	// Do nothing for now, might change later.
}
