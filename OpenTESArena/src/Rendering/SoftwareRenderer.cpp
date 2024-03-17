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

// Loop unroll utils.
namespace
{
	constexpr int TYPICAL_LOOP_UNROLL = 4; // Elements processed per unrolled loop, possibly also for SIMD lanes.
	constexpr int WEAK_LOOP_UNROLL = TYPICAL_LOOP_UNROLL / 2;
	constexpr int AGGRESSIVE_LOOP_UNROLL = TYPICAL_LOOP_UNROLL * 2;
	static_assert(MathUtils::isPowerOf2(TYPICAL_LOOP_UNROLL));
	static_assert(MathUtils::isPowerOf2(WEAK_LOOP_UNROLL));
	static_assert(MathUtils::isPowerOf2(AGGRESSIVE_LOOP_UNROLL));

	int GetUnrollAdjustedLoopCount(int loopCount, int unrollCount)
	{
		return loopCount - (unrollCount - 1);
	}
}

// Optimized math functions.
namespace
{
	template<int N>
	void Double_LerpN(const double *__restrict starts, const double *__restrict ends, const double *__restrict percents,
		double *__restrict outs)
	{
		for (int i = 0; i < N; i++)
		{
			const double start = starts[i];
			const double end = ends[i];
			const double percent = percents[i];
			outs[i] = start + ((end - start) * percent);
		}
	}

	template<int Index0, int Index1, int Index2, int Index3>
	void Double_Shuffle4(const double *__restrict values, double *__restrict outValues)
	{
		outValues[Index0] = values[0];
		outValues[Index1] = values[1];
		outValues[Index2] = values[2];
		outValues[Index3] = values[3];
	}

	template<int N>
	void Double2_DotN(const double *__restrict x0s, const double *__restrict y0s, const double *__restrict x1s, const double *__restrict y1s,
		double *__restrict outs)
	{
		for (int i = 0; i < N; i++)
		{
			outs[i] = (x0s[i] * x1s[i]) + (y0s[i] * y1s[i]);
		}
	}

	template<int N>
	void Double2_CrossN(const double *__restrict x0s, const double *__restrict y0s, const double *__restrict x1s, const double *__restrict y1s,
		double *__restrict outs)
	{
		for (int i = 0; i < N; i++)
		{
			outs[i] = (x0s[i] * y1s[i]) - (y0s[i] * x1s[i]);
		}
	}

	template<int N>
	void Double2_RightPerpN(const double *__restrict xs, const double *__restrict ys, double *__restrict outXs, double *__restrict outYs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = ys[i];
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = -xs[i];
		}
	}

	template<int N>
	void Double4_ZeroN(double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] = 0.0;
		}
	}

	void Double4_Load4(const Double4 &v0, const Double4 &v1, const Double4 &v2, const Double4 &v3,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		outXs[0] = v0.x;
		outYs[0] = v0.y;
		outZs[0] = v0.z;
		outWs[0] = v0.w;
		outXs[1] = v1.x;
		outYs[1] = v1.y;
		outZs[1] = v1.z;
		outWs[1] = v1.w;
		outXs[2] = v2.x;
		outYs[2] = v2.y;
		outZs[2] = v2.z;
		outWs[2] = v2.w;
		outXs[3] = v3.x;
		outYs[3] = v3.y;
		outZs[3] = v3.z;
		outWs[3] = v3.w;
	}

	void Double4_Store4(const double *__restrict xs, const double *__restrict ys, const double *__restrict zs, const double *__restrict ws,
		double *__restrict outV0, double *__restrict outV1, double *__restrict outV2, double *__restrict outV3)
	{
		outV0[0] = xs[0];
		outV0[1] = ys[0];
		outV0[2] = zs[0];
		outV0[3] = ws[0];
		outV1[0] = xs[1];
		outV1[1] = ys[1];
		outV1[2] = zs[1];
		outV1[3] = ws[1];
		outV2[0] = xs[2];
		outV2[1] = ys[2];
		outV2[2] = zs[2];
		outV2[3] = ws[2];
		outV3[0] = xs[3];
		outV3[1] = ys[3];
		outV3[2] = zs[3];
		outV3[3] = ws[3];
	}

	template<int N>
	void Double4_AddN(const double *__restrict x0s, const double *__restrict y0s, const double *__restrict z0s, const double *__restrict w0s,
		const double *__restrict x1s, const double *__restrict y1s, const double *__restrict z1s, const double *__restrict w1s,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = x0s[i] + x1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = y0s[i] + y1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] = z0s[i] + z1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] = w0s[i] + w1s[i];
		}
	}

	template<int N>
	void Double4_NegateN(const double *__restrict xs, const double *__restrict ys, const double *__restrict zs, const double *__restrict ws,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = -xs[i];
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = -ys[i];
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] = -zs[i];
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] = -ws[i];
		}
	}

	template<int N>
	void Double4_SubtractN(const double *__restrict x0s, const double *__restrict y0s, const double *__restrict z0s, const double *__restrict w0s,
		const double *__restrict x1s, const double *__restrict y1s, const double *__restrict z1s, const double *__restrict w1s,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = x0s[i] - x1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = y0s[i] - y1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] = z0s[i] - z1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] = w0s[i] - w1s[i];
		}
	}

	template<int N>
	void Double4_MultiplyN(const double *__restrict x0s, const double *__restrict y0s, const double *__restrict z0s, const double *__restrict w0s,
		const double *__restrict x1s, const double *__restrict y1s, const double *__restrict z1s, const double *__restrict w1s,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = x0s[i] * x1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = y0s[i] * y1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] = z0s[i] * z1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] = w0s[i] * w1s[i];
		}
	}

	template<int N>
	void Double4_DivideN(const double *__restrict x0s, const double *__restrict y0s, const double *__restrict z0s, const double *__restrict w0s,
		const double *__restrict x1s, const double *__restrict y1s, const double *__restrict z1s, const double *__restrict w1s,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] = x0s[i] / x1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] = y0s[i] / y1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] = z0s[i] / z1s[i];
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] = w0s[i] / w1s[i];
		}
	}

	template<int N>
	void Matrix4_ZeroN(double *__restrict outMxxs, double *__restrict outMxys, double *__restrict outMxzs, double *__restrict outMxws,
		double *__restrict outMyxs, double *__restrict outMyys, double *__restrict outMyzs, double *__restrict outMyws,
		double *__restrict outMzxs, double *__restrict outMzys, double *__restrict outMzzs, double *__restrict outMzws,
		double *__restrict outMwxs, double *__restrict outMwys, double *__restrict outMwzs, double *__restrict outMwws)
	{
		for (int i = 0; i < N; i++)
		{
			outMxxs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMxys[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMxzs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMxws[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMyxs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMyys[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMyzs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMyws[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMzxs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMzys[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMzzs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMzws[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMwxs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMwys[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMwzs[i] = 0.0;
		}

		for (int i = 0; i < N; i++)
		{
			outMwws[i] = 0.0;
		}
	}

	void Matrix4_Load4(const Matrix4d &m0, const Matrix4d &m1, const Matrix4d &m2, const Matrix4d &m3,
		double *__restrict outMxxs, double *__restrict outMxys, double *__restrict outMxzs, double *__restrict outMxws,
		double *__restrict outMyxs, double *__restrict outMyys, double *__restrict outMyzs, double *__restrict outMyws,
		double *__restrict outMzxs, double *__restrict outMzys, double *__restrict outMzzs, double *__restrict outMzws,
		double *__restrict outMwxs, double *__restrict outMwys, double *__restrict outMwzs, double *__restrict outMwws)
	{
		outMxxs[0] = m0.x.x;
		outMxys[0] = m0.x.y;
		outMxzs[0] = m0.x.z;
		outMxws[0] = m0.x.w;
		outMyxs[0] = m0.y.x;
		outMyys[0] = m0.y.y;
		outMyzs[0] = m0.y.z;
		outMyws[0] = m0.y.w;
		outMzxs[0] = m0.z.x;
		outMzys[0] = m0.z.y;
		outMzzs[0] = m0.z.z;
		outMzws[0] = m0.z.w;
		outMwxs[0] = m0.w.x;
		outMwys[0] = m0.w.y;
		outMwzs[0] = m0.w.z;
		outMwws[0] = m0.w.w;
		outMxxs[1] = m1.x.x;
		outMxys[1] = m1.x.y;
		outMxzs[1] = m1.x.z;
		outMxws[1] = m1.x.w;
		outMyxs[1] = m1.y.x;
		outMyys[1] = m1.y.y;
		outMyzs[1] = m1.y.z;
		outMyws[1] = m1.y.w;
		outMzxs[1] = m1.z.x;
		outMzys[1] = m1.z.y;
		outMzzs[1] = m1.z.z;
		outMzws[1] = m1.z.w;
		outMwxs[1] = m1.w.x;
		outMwys[1] = m1.w.y;
		outMwzs[1] = m1.w.z;
		outMwws[1] = m1.w.w;
		outMxxs[2] = m2.x.x;
		outMxys[2] = m2.x.y;
		outMxzs[2] = m2.x.z;
		outMxws[2] = m2.x.w;
		outMyxs[2] = m2.y.x;
		outMyys[2] = m2.y.y;
		outMyzs[2] = m2.y.z;
		outMyws[2] = m2.y.w;
		outMzxs[2] = m2.z.x;
		outMzys[2] = m2.z.y;
		outMzzs[2] = m2.z.z;
		outMzws[2] = m2.z.w;
		outMwxs[2] = m2.w.x;
		outMwys[2] = m2.w.y;
		outMwzs[2] = m2.w.z;
		outMwws[2] = m2.w.w;
		outMxxs[3] = m3.x.x;
		outMxys[3] = m3.x.y;
		outMxzs[3] = m3.x.z;
		outMxws[3] = m3.x.w;
		outMyxs[3] = m3.y.x;
		outMyys[3] = m3.y.y;
		outMyzs[3] = m3.y.z;
		outMyws[3] = m3.y.w;
		outMzxs[3] = m3.z.x;
		outMzys[3] = m3.z.y;
		outMzzs[3] = m3.z.z;
		outMzws[3] = m3.z.w;
		outMwxs[3] = m3.w.x;
		outMwys[3] = m3.w.y;
		outMwzs[3] = m3.w.z;
		outMwws[3] = m3.w.w;
	}

	template<int N>
	void Matrix4_MultiplyVectorN(
		const double *__restrict mxxs, const double *__restrict mxys, const double *__restrict mxzs, const double *__restrict mxws,
		const double *__restrict myxs, const double *__restrict myys, const double *__restrict myzs, const double *__restrict myws,
		const double *__restrict mzxs, const double *__restrict mzys, const double *__restrict mzzs, const double *__restrict mzws,
		const double *__restrict mwxs, const double *__restrict mwys, const double *__restrict mwzs, const double *__restrict mwws,
		const double *__restrict xs, const double *__restrict ys, const double *__restrict zs, const double *__restrict ws,
		double *__restrict outXs, double *__restrict outYs, double *__restrict outZs, double *__restrict outWs)
	{
		for (int i = 0; i < N; i++)
		{
			outXs[i] += (mxxs[i] * xs[i]) + (myxs[i] * ys[i]) + (mzxs[i] * zs[i]) + (mwxs[i] * ws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outYs[i] += (mxys[i] * xs[i]) + (myys[i] * ys[i]) + (mzys[i] * zs[i]) + (mwys[i] * ws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outZs[i] += (mxzs[i] * xs[i]) + (myzs[i] * ys[i]) + (mzzs[i] * zs[i]) + (mwzs[i] * ws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outWs[i] += (mxws[i] * xs[i]) + (myws[i] * ys[i]) + (mzws[i] * zs[i]) + (mwws[i] * ws[i]);
		}
	}

	template<int N>
	void Matrix4_MultiplyMatrixN(const double *__restrict m0xxs, const double *__restrict m0xys, const double *__restrict m0xzs, const double *__restrict m0xws,
		const double *__restrict m0yxs, const double *__restrict m0yys, const double *__restrict m0yzs, const double *__restrict m0yws,
		const double *__restrict m0zxs, const double *__restrict m0zys, const double *__restrict m0zzs, const double *__restrict m0zws,
		const double *__restrict m0wxs, const double *__restrict m0wys, const double *__restrict m0wzs, const double *__restrict m0wws,
		const double *__restrict m1xxs, const double *__restrict m1xys, const double *__restrict m1xzs, const double *__restrict m1xws,
		const double *__restrict m1yxs, const double *__restrict m1yys, const double *__restrict m1yzs, const double *__restrict m1yws,
		const double *__restrict m1zxs, const double *__restrict m1zys, const double *__restrict m1zzs, const double *__restrict m1zws,
		const double *__restrict m1wxs, const double *__restrict m1wys, const double *__restrict m1wzs, const double *__restrict m1wws,
		double *__restrict outMxxs, double *__restrict outMxys, double *__restrict outMxzs, double *__restrict outMxws,
		double *__restrict outMyxs, double *__restrict outMyys, double *__restrict outMyzs, double *__restrict outMyws,
		double *__restrict outMzxs, double *__restrict outMzys, double *__restrict outMzzs, double *__restrict outMzws,
		double *__restrict outMwxs, double *__restrict outMwys, double *__restrict outMwzs, double *__restrict outMwws)
	{
		for (int i = 0; i < N; i++)
		{
			outMxxs[i] = (m0xxs[i] * m1xxs[i]) + (m0yxs[i] * m1xys[i]) + (m0zxs[i] * m1xzs[i]) + (m0wxs[i] * m1xws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMxys[i] = (m0xys[i] * m1xxs[i]) + (m0yys[i] * m1xys[i]) + (m0zys[i] * m1xzs[i]) + (m0wys[i] * m1xws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMxzs[i] = (m0xzs[i] * m1xxs[i]) + (m0yzs[i] * m1xys[i]) + (m0zzs[i] * m1xzs[i]) + (m0wzs[i] * m1xws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMxws[i] = (m0xws[i] * m1xxs[i]) + (m0yws[i] * m1xys[i]) + (m0zws[i] * m1xzs[i]) + (m0wws[i] * m1xws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMyxs[i] = (m0xxs[i] * m1yxs[i]) + (m0yxs[i] * m1yys[i]) + (m0zxs[i] * m1yzs[i]) + (m0wxs[i] * m1yws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMyys[i] = (m0xys[i] * m1yxs[i]) + (m0yys[i] * m1yys[i]) + (m0zys[i] * m1yzs[i]) + (m0wys[i] * m1yws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMyzs[i] = (m0xzs[i] * m1yxs[i]) + (m0yzs[i] * m1yys[i]) + (m0zzs[i] * m1yzs[i]) + (m0wzs[i] * m1yws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMyws[i] = (m0xws[i] * m1yxs[i]) + (m0yws[i] * m1yys[i]) + (m0zws[i] * m1yzs[i]) + (m0wws[i] * m1yws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMzxs[i] = (m0xxs[i] * m1zxs[i]) + (m0yxs[i] * m1zys[i]) + (m0zxs[i] * m1zzs[i]) + (m0wxs[i] * m1zws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMzys[i] = (m0xys[i] * m1zxs[i]) + (m0yys[i] * m1zys[i]) + (m0zys[i] * m1zzs[i]) + (m0wys[i] * m1zws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMzzs[i] = (m0xzs[i] * m1zxs[i]) + (m0yzs[i] * m1zys[i]) + (m0zzs[i] * m1zzs[i]) + (m0wzs[i] * m1zws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMzws[i] = (m0xws[i] * m1zxs[i]) + (m0yws[i] * m1zys[i]) + (m0zws[i] * m1zzs[i]) + (m0wws[i] * m1zws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMwxs[i] = (m0xxs[i] * m1wxs[i]) + (m0yxs[i] * m1wys[i]) + (m0zxs[i] * m1wzs[i]) + (m0wxs[i] * m1wws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMwys[i] = (m0xys[i] * m1wxs[i]) + (m0yys[i] * m1wys[i]) + (m0zys[i] * m1wzs[i]) + (m0wys[i] * m1wws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMwzs[i] = (m0xzs[i] * m1wxs[i]) + (m0yzs[i] * m1wys[i]) + (m0zzs[i] * m1wzs[i]) + (m0wzs[i] * m1wws[i]);
		}

		for (int i = 0; i < N; i++)
		{
			outMwws[i] = (m0xws[i] * m1wxs[i]) + (m0yws[i] * m1wys[i]) + (m0zws[i] * m1wzs[i]) + (m0wws[i] * m1wws[i]);
		}
	}
}

// Rasterization utils.
namespace
{
	double NdcXToScreenSpace(double ndcX, double frameWidth)
	{
		return (0.50 + (ndcX * 0.50)) * frameWidth;
	}

	double NdcYToScreenSpace(double ndcY, double frameHeight)
	{
		return (0.50 - (ndcY * 0.50)) * frameHeight;
	}

	bool IsScreenSpacePointInHalfSpace(double pointX, double pointY, double planePointX, double planePointY, double planeNormalX, double planeNormalY)
	{
		const double pointXDiff = pointX - planePointX;
		const double pointYDiff = pointY - planePointY;
		const double dotProduct = (pointXDiff * planeNormalX) + (pointYDiff * planeNormalY);
		return dotProduct >= 0.0;
	}
}

// Camera globals.
namespace
{
	Double3 g_horizonNdcPoint; // For horizon reflections.

	Matrix4d g_viewMatrix;
	Matrix4d g_projMatrix;

	Matrix4d g_viewProjMatrix;
	double g_viewProjMatrixXX[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixXY[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixXZ[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixXW[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixYX[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixYY[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixYZ[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixYW[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixZX[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixZY[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixZZ[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixZW[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixWX[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixWY[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixWZ[TYPICAL_LOOP_UNROLL];
	double g_viewProjMatrixWW[TYPICAL_LOOP_UNROLL];

	Matrix4d g_invViewMatrix;
	double g_invViewMatrixXX[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixXY[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixXZ[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixXW[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixYX[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixYY[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixYZ[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixYW[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixZX[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixZY[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixZZ[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixZW[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixWX[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixWY[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixWZ[TYPICAL_LOOP_UNROLL];
	double g_invViewMatrixWW[TYPICAL_LOOP_UNROLL];

	Matrix4d g_invProjMatrix;
	double g_invProjMatrixXX[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixXY[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixXZ[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixXW[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixYX[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixYY[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixYZ[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixYW[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixZX[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixZY[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixZZ[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixZW[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixWX[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixWY[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixWZ[TYPICAL_LOOP_UNROLL];
	double g_invProjMatrixWW[TYPICAL_LOOP_UNROLL];

	void PopulateCameraGlobals(const RenderCamera &camera)
	{
		g_horizonNdcPoint = camera.horizonNdcPoint;
		g_viewMatrix = camera.viewMatrix;
		g_projMatrix = camera.projectionMatrix;
		g_viewProjMatrix = camera.projectionMatrix * camera.viewMatrix;
		g_invViewMatrix = camera.inverseViewMatrix;
		g_invProjMatrix = camera.inverseProjectionMatrix;

		for (int i = 0; i < TYPICAL_LOOP_UNROLL; i++)
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

			g_invViewMatrixXX[i] = g_invViewMatrix.x.x;
			g_invViewMatrixXY[i] = g_invViewMatrix.x.y;
			g_invViewMatrixXZ[i] = g_invViewMatrix.x.z;
			g_invViewMatrixXW[i] = g_invViewMatrix.x.w;
			g_invViewMatrixYX[i] = g_invViewMatrix.y.x;
			g_invViewMatrixYY[i] = g_invViewMatrix.y.y;
			g_invViewMatrixYZ[i] = g_invViewMatrix.y.z;
			g_invViewMatrixYW[i] = g_invViewMatrix.y.w;
			g_invViewMatrixZX[i] = g_invViewMatrix.z.x;
			g_invViewMatrixZY[i] = g_invViewMatrix.z.y;
			g_invViewMatrixZZ[i] = g_invViewMatrix.z.z;
			g_invViewMatrixZW[i] = g_invViewMatrix.z.w;
			g_invViewMatrixWX[i] = g_invViewMatrix.w.x;
			g_invViewMatrixWY[i] = g_invViewMatrix.w.y;
			g_invViewMatrixWZ[i] = g_invViewMatrix.w.z;
			g_invViewMatrixWW[i] = g_invViewMatrix.w.w;

			g_invProjMatrixXX[i] = g_invProjMatrix.x.x;
			g_invProjMatrixXY[i] = g_invProjMatrix.x.y;
			g_invProjMatrixXZ[i] = g_invProjMatrix.x.z;
			g_invProjMatrixXW[i] = g_invProjMatrix.x.w;
			g_invProjMatrixYX[i] = g_invProjMatrix.y.x;
			g_invProjMatrixYY[i] = g_invProjMatrix.y.y;
			g_invProjMatrixYZ[i] = g_invProjMatrix.y.z;
			g_invProjMatrixYW[i] = g_invProjMatrix.y.w;
			g_invProjMatrixZX[i] = g_invProjMatrix.z.x;
			g_invProjMatrixZY[i] = g_invProjMatrix.z.y;
			g_invProjMatrixZZ[i] = g_invProjMatrix.z.z;
			g_invProjMatrixZW[i] = g_invProjMatrix.z.w;
			g_invProjMatrixWX[i] = g_invProjMatrix.w.x;
			g_invProjMatrixWY[i] = g_invProjMatrix.w.y;
			g_invProjMatrixWZ[i] = g_invProjMatrix.w.z;
			g_invProjMatrixWW[i] = g_invProjMatrix.w.w;
		}
	}
}

// Mesh processing globals.
namespace
{
	constexpr int MAX_DRAW_CALL_MESH_TRIANGLES = 1024; // The most triangles a draw call mesh can have. Used with vertex shading.
	constexpr int MAX_MESH_PROCESS_CACHES = 8; // The most draw call meshes that can be cached and processed each loop.
	constexpr int MAX_VERTEX_SHADING_CACHE_TRIANGLES = MAX_DRAW_CALL_MESH_TRIANGLES * 2; // The most unshaded triangles that can be cached for the vertex shader loop.
	constexpr int MAX_CLIPPED_MESH_TRIANGLES = 4096; // The most triangles a processed clip space mesh can have when passed to the rasterizer.
	constexpr int MAX_CLIPPED_TRIANGLE_TRIANGLES = 64; // The most triangles a triangle can generate after being clipped by all clip planes.

	// Bulk draw call processing caches sharing a vertex shader to calculate clipped meshes for rasterizing.
	// Struct-of-arrays layout for speed.
	struct MeshProcessCaches
	{
		// Transform matrices for each mesh.
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
		double shadedV0XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES][4];
		double shadedV1XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES][4];
		double shadedV2XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES][4];
		double uv0XYArrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES][2];
		double uv1XYArrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES][2];
		double uv2XYArrays[MAX_MESH_PROCESS_CACHES][MAX_DRAW_CALL_MESH_TRIANGLES][2];
		int triangleWriteCounts[MAX_MESH_PROCESS_CACHES]; // This should match the draw call triangle count.

		// Triangles generated by clipping the current mesh. These are sent to the rasterizer.
		double clipSpaceMeshV0XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES][4];
		double clipSpaceMeshV1XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES][4];
		double clipSpaceMeshV2XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES][4];
		double clipSpaceMeshUV0XYArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES][2];
		double clipSpaceMeshUV1XYArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES][2];
		double clipSpaceMeshUV2XYArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES][2];

		// Triangles generated by clipping the current triangle against clipping planes.
		double clipSpaceTriangleV0XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES][4];
		double clipSpaceTriangleV1XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES][4];
		double clipSpaceTriangleV2XYZWArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES][4];
		double clipSpaceTriangleUV0XYArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES][2];
		double clipSpaceTriangleUV1XYArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES][2];
		double clipSpaceTriangleUV2XYArrays[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_TRIANGLE_TRIANGLES][2];

		// Triangles in the current clip space mesh to be rasterized.
		int clipSpaceMeshTriangleCounts[MAX_MESH_PROCESS_CACHES];
	};

	MeshProcessCaches g_meshProcessCaches;

	void PopulateMeshTransform(int meshIndex, const RenderTransform &transform)
	{
		g_meshProcessCaches.translationMatrixXXs[meshIndex] = transform.translation.x.x;
		g_meshProcessCaches.translationMatrixXYs[meshIndex] = transform.translation.x.y;
		g_meshProcessCaches.translationMatrixXZs[meshIndex] = transform.translation.x.z;
		g_meshProcessCaches.translationMatrixXWs[meshIndex] = transform.translation.x.w;
		g_meshProcessCaches.translationMatrixYXs[meshIndex] = transform.translation.y.x;
		g_meshProcessCaches.translationMatrixYYs[meshIndex] = transform.translation.y.y;
		g_meshProcessCaches.translationMatrixYZs[meshIndex] = transform.translation.y.z;
		g_meshProcessCaches.translationMatrixYWs[meshIndex] = transform.translation.y.w;
		g_meshProcessCaches.translationMatrixZXs[meshIndex] = transform.translation.z.x;
		g_meshProcessCaches.translationMatrixZYs[meshIndex] = transform.translation.z.y;
		g_meshProcessCaches.translationMatrixZZs[meshIndex] = transform.translation.z.z;
		g_meshProcessCaches.translationMatrixZWs[meshIndex] = transform.translation.z.w;
		g_meshProcessCaches.translationMatrixWXs[meshIndex] = transform.translation.w.x;
		g_meshProcessCaches.translationMatrixWYs[meshIndex] = transform.translation.w.y;
		g_meshProcessCaches.translationMatrixWZs[meshIndex] = transform.translation.w.z;
		g_meshProcessCaches.translationMatrixWWs[meshIndex] = transform.translation.w.w;
		g_meshProcessCaches.rotationMatrixXXs[meshIndex] = transform.rotation.x.x;
		g_meshProcessCaches.rotationMatrixXYs[meshIndex] = transform.rotation.x.y;
		g_meshProcessCaches.rotationMatrixXZs[meshIndex] = transform.rotation.x.z;
		g_meshProcessCaches.rotationMatrixXWs[meshIndex] = transform.rotation.x.w;
		g_meshProcessCaches.rotationMatrixYXs[meshIndex] = transform.rotation.y.x;
		g_meshProcessCaches.rotationMatrixYYs[meshIndex] = transform.rotation.y.y;
		g_meshProcessCaches.rotationMatrixYZs[meshIndex] = transform.rotation.y.z;
		g_meshProcessCaches.rotationMatrixYWs[meshIndex] = transform.rotation.y.w;
		g_meshProcessCaches.rotationMatrixZXs[meshIndex] = transform.rotation.z.x;
		g_meshProcessCaches.rotationMatrixZYs[meshIndex] = transform.rotation.z.y;
		g_meshProcessCaches.rotationMatrixZZs[meshIndex] = transform.rotation.z.z;
		g_meshProcessCaches.rotationMatrixZWs[meshIndex] = transform.rotation.z.w;
		g_meshProcessCaches.rotationMatrixWXs[meshIndex] = transform.rotation.w.x;
		g_meshProcessCaches.rotationMatrixWYs[meshIndex] = transform.rotation.w.y;
		g_meshProcessCaches.rotationMatrixWZs[meshIndex] = transform.rotation.w.z;
		g_meshProcessCaches.rotationMatrixWWs[meshIndex] = transform.rotation.w.w;
		g_meshProcessCaches.scaleMatrixXXs[meshIndex] = transform.scale.x.x;
		g_meshProcessCaches.scaleMatrixXYs[meshIndex] = transform.scale.x.y;
		g_meshProcessCaches.scaleMatrixXZs[meshIndex] = transform.scale.x.z;
		g_meshProcessCaches.scaleMatrixXWs[meshIndex] = transform.scale.x.w;
		g_meshProcessCaches.scaleMatrixYXs[meshIndex] = transform.scale.y.x;
		g_meshProcessCaches.scaleMatrixYYs[meshIndex] = transform.scale.y.y;
		g_meshProcessCaches.scaleMatrixYZs[meshIndex] = transform.scale.y.z;
		g_meshProcessCaches.scaleMatrixYWs[meshIndex] = transform.scale.y.w;
		g_meshProcessCaches.scaleMatrixZXs[meshIndex] = transform.scale.z.x;
		g_meshProcessCaches.scaleMatrixZYs[meshIndex] = transform.scale.z.y;
		g_meshProcessCaches.scaleMatrixZZs[meshIndex] = transform.scale.z.z;
		g_meshProcessCaches.scaleMatrixZWs[meshIndex] = transform.scale.z.w;
		g_meshProcessCaches.scaleMatrixWXs[meshIndex] = transform.scale.w.x;
		g_meshProcessCaches.scaleMatrixWYs[meshIndex] = transform.scale.w.y;
		g_meshProcessCaches.scaleMatrixWZs[meshIndex] = transform.scale.w.z;
		g_meshProcessCaches.scaleMatrixWWs[meshIndex] = transform.scale.w.w;
		// Do model-view-projection matrix in the bulk processing loop.
	}
}

// Frame buffer globals.
namespace
{
	int g_frameBufferWidth;
	int g_frameBufferHeight;
	int g_frameBufferPixelCount;
	double g_frameBufferWidthReal;
	double g_frameBufferHeightReal;
	double g_frameBufferWidthRealRecip;
	double g_frameBufferHeightRealRecip;
	int g_ditherBufferDepth;
	DitheringMode g_ditheringMode;
	uint8_t *g_paletteIndexBuffer;
	double *g_depthBuffer;
	const bool *g_ditherBuffer;
	uint32_t *g_colorBuffer;

	void PopulateRasterizerGlobals(int frameBufferWidth, int frameBufferHeight, uint8_t *paletteIndexBuffer, double *depthBuffer,
		const bool *ditherBuffer, int ditherBufferDepth, DitheringMode ditheringMode, uint32_t *colorBuffer)
	{
		g_frameBufferWidth = frameBufferWidth;
		g_frameBufferHeight = frameBufferHeight;
		g_frameBufferPixelCount = frameBufferWidth * frameBufferHeight;
		g_frameBufferWidthReal = static_cast<double>(frameBufferWidth);
		g_frameBufferHeightReal = static_cast<double>(frameBufferHeight);
		g_frameBufferWidthRealRecip = 1.0 / g_frameBufferWidthReal;
		g_frameBufferHeightRealRecip = 1.0 / g_frameBufferHeightReal;
		g_ditherBufferDepth = ditherBufferDepth;
		g_ditheringMode = ditheringMode;
		g_paletteIndexBuffer = paletteIndexBuffer;
		g_depthBuffer = depthBuffer;
		g_ditherBuffer = ditherBuffer;
		g_colorBuffer = colorBuffer;
	}

	// For measuring overdraw.
	int g_totalDepthTests = 0;
	int g_totalColorWrites = 0;

	void ClearFrameBuffers()
	{
		std::fill(g_paletteIndexBuffer, g_paletteIndexBuffer + g_frameBufferPixelCount, 0);
		std::fill(g_depthBuffer, g_depthBuffer + g_frameBufferPixelCount, std::numeric_limits<double>::infinity());
		std::fill(g_colorBuffer, g_colorBuffer + g_frameBufferPixelCount, 0);
		g_totalDepthTests = 0;
		g_totalColorWrites = 0;
	}

	void CreateDitherBuffer(Buffer3D<bool> &ditherBuffer, int width, int height, DitheringMode ditheringMode)
	{
		if (ditheringMode == DitheringMode::Classic)
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
		else if (ditheringMode == DitheringMode::Modern)
		{
			// Modern 2x2, four levels of dither depending on percent between two light levels.
			ditherBuffer.init(width, height, DITHERING_MODERN_MASK_COUNT);
			static_assert(DITHERING_MODERN_MASK_COUNT == 4);

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
}

// Vertex shaders.
namespace
{
	template<int N>
	void VertexShader_BasicN(const int *__restrict meshIndices, const double *__restrict vertexXs, const double *__restrict vertexYs,
		const double *__restrict vertexZs, const double *__restrict vertexWs, double *__restrict outVertexXs, double *__restrict outVertexYs,
		double *__restrict outVertexZs, double *__restrict outVertexWs)
	{
		// Read in each mesh's transforms.
		double modelViewProjMatrixXXs[N];
		double modelViewProjMatrixXYs[N];
		double modelViewProjMatrixXZs[N];
		double modelViewProjMatrixXWs[N];
		double modelViewProjMatrixYXs[N];
		double modelViewProjMatrixYYs[N];
		double modelViewProjMatrixYZs[N];
		double modelViewProjMatrixYWs[N];
		double modelViewProjMatrixZXs[N];
		double modelViewProjMatrixZYs[N];
		double modelViewProjMatrixZZs[N];
		double modelViewProjMatrixZWs[N];
		double modelViewProjMatrixWXs[N];
		double modelViewProjMatrixWYs[N];
		double modelViewProjMatrixWZs[N];
		double modelViewProjMatrixWWs[N];
		for (int i = 0; i < N; i++)
		{
			const int meshIndex = meshIndices[i];
			modelViewProjMatrixXXs[i] = g_meshProcessCaches.modelViewProjMatrixXXs[meshIndex];
			modelViewProjMatrixXYs[i] = g_meshProcessCaches.modelViewProjMatrixXYs[meshIndex];
			modelViewProjMatrixXZs[i] = g_meshProcessCaches.modelViewProjMatrixXZs[meshIndex];
			modelViewProjMatrixXWs[i] = g_meshProcessCaches.modelViewProjMatrixXWs[meshIndex];
			modelViewProjMatrixYXs[i] = g_meshProcessCaches.modelViewProjMatrixYXs[meshIndex];
			modelViewProjMatrixYYs[i] = g_meshProcessCaches.modelViewProjMatrixYYs[meshIndex];
			modelViewProjMatrixYZs[i] = g_meshProcessCaches.modelViewProjMatrixYZs[meshIndex];
			modelViewProjMatrixYWs[i] = g_meshProcessCaches.modelViewProjMatrixYWs[meshIndex];
			modelViewProjMatrixZXs[i] = g_meshProcessCaches.modelViewProjMatrixZXs[meshIndex];
			modelViewProjMatrixZYs[i] = g_meshProcessCaches.modelViewProjMatrixZYs[meshIndex];
			modelViewProjMatrixZZs[i] = g_meshProcessCaches.modelViewProjMatrixZZs[meshIndex];
			modelViewProjMatrixZWs[i] = g_meshProcessCaches.modelViewProjMatrixZWs[meshIndex];
			modelViewProjMatrixWXs[i] = g_meshProcessCaches.modelViewProjMatrixWXs[meshIndex];
			modelViewProjMatrixWYs[i] = g_meshProcessCaches.modelViewProjMatrixWYs[meshIndex];
			modelViewProjMatrixWZs[i] = g_meshProcessCaches.modelViewProjMatrixWZs[meshIndex];
			modelViewProjMatrixWWs[i] = g_meshProcessCaches.modelViewProjMatrixWWs[meshIndex];
		}

		// Apply model-view-projection matrix.
		Matrix4_MultiplyVectorN<N>(
			modelViewProjMatrixXXs, modelViewProjMatrixXYs, modelViewProjMatrixXZs, modelViewProjMatrixXWs,
			modelViewProjMatrixYXs, modelViewProjMatrixYYs, modelViewProjMatrixYZs, modelViewProjMatrixYWs,
			modelViewProjMatrixZXs, modelViewProjMatrixZYs, modelViewProjMatrixZZs, modelViewProjMatrixZWs,
			modelViewProjMatrixWXs, modelViewProjMatrixWYs, modelViewProjMatrixWZs, modelViewProjMatrixWWs,
			vertexXs, vertexYs, vertexZs, vertexWs,
			outVertexXs, outVertexYs, outVertexZs, outVertexWs);
	}

	template<int N>
	void VertexShader_RaisingDoorN(const int *__restrict meshIndices, const double *__restrict vertexXs, const double *__restrict vertexYs,
		const double *__restrict vertexZs, const double *__restrict vertexWs, double *__restrict outVertexXs, double *__restrict outVertexYs,
		double *__restrict outVertexZs, double *__restrict outVertexWs)
	{
		// Read in each mesh's transforms.
		double preScaleTranslationXs[N];
		double preScaleTranslationYs[N];
		double preScaleTranslationZs[N];
		double translationMatrixXXs[N];
		double translationMatrixXYs[N];
		double translationMatrixXZs[N];
		double translationMatrixXWs[N];
		double translationMatrixYXs[N];
		double translationMatrixYYs[N];
		double translationMatrixYZs[N];
		double translationMatrixYWs[N];
		double translationMatrixZXs[N];
		double translationMatrixZYs[N];
		double translationMatrixZZs[N];
		double translationMatrixZWs[N];
		double translationMatrixWXs[N];
		double translationMatrixWYs[N];
		double translationMatrixWZs[N];
		double translationMatrixWWs[N];
		double rotationMatrixXXs[N];
		double rotationMatrixXYs[N];
		double rotationMatrixXZs[N];
		double rotationMatrixXWs[N];
		double rotationMatrixYXs[N];
		double rotationMatrixYYs[N];
		double rotationMatrixYZs[N];
		double rotationMatrixYWs[N];
		double rotationMatrixZXs[N];
		double rotationMatrixZYs[N];
		double rotationMatrixZZs[N];
		double rotationMatrixZWs[N];
		double rotationMatrixWXs[N];
		double rotationMatrixWYs[N];
		double rotationMatrixWZs[N];
		double rotationMatrixWWs[N];
		double scaleMatrixXXs[N];
		double scaleMatrixXYs[N];
		double scaleMatrixXZs[N];
		double scaleMatrixXWs[N];
		double scaleMatrixYXs[N];
		double scaleMatrixYYs[N];
		double scaleMatrixYZs[N];
		double scaleMatrixYWs[N];
		double scaleMatrixZXs[N];
		double scaleMatrixZYs[N];
		double scaleMatrixZZs[N];
		double scaleMatrixZWs[N];
		double scaleMatrixWXs[N];
		double scaleMatrixWYs[N];
		double scaleMatrixWZs[N];
		double scaleMatrixWWs[N];
		for (int i = 0; i < N; i++)
		{
			const int meshIndex = meshIndices[i];
			preScaleTranslationXs[i] = g_meshProcessCaches.preScaleTranslationXs[meshIndex];
			preScaleTranslationYs[i] = g_meshProcessCaches.preScaleTranslationYs[meshIndex];
			preScaleTranslationZs[i] = g_meshProcessCaches.preScaleTranslationZs[meshIndex];
			translationMatrixXXs[i] = g_meshProcessCaches.translationMatrixXXs[meshIndex];
			translationMatrixXYs[i] = g_meshProcessCaches.translationMatrixXYs[meshIndex];
			translationMatrixXZs[i] = g_meshProcessCaches.translationMatrixXZs[meshIndex];
			translationMatrixXWs[i] = g_meshProcessCaches.translationMatrixXWs[meshIndex];
			translationMatrixYXs[i] = g_meshProcessCaches.translationMatrixYXs[meshIndex];
			translationMatrixYYs[i] = g_meshProcessCaches.translationMatrixYYs[meshIndex];
			translationMatrixYZs[i] = g_meshProcessCaches.translationMatrixYZs[meshIndex];
			translationMatrixYWs[i] = g_meshProcessCaches.translationMatrixYWs[meshIndex];
			translationMatrixZXs[i] = g_meshProcessCaches.translationMatrixZXs[meshIndex];
			translationMatrixZYs[i] = g_meshProcessCaches.translationMatrixZYs[meshIndex];
			translationMatrixZZs[i] = g_meshProcessCaches.translationMatrixZZs[meshIndex];
			translationMatrixZWs[i] = g_meshProcessCaches.translationMatrixZWs[meshIndex];
			translationMatrixWXs[i] = g_meshProcessCaches.translationMatrixWXs[meshIndex];
			translationMatrixWYs[i] = g_meshProcessCaches.translationMatrixWYs[meshIndex];
			translationMatrixWZs[i] = g_meshProcessCaches.translationMatrixWZs[meshIndex];
			translationMatrixWWs[i] = g_meshProcessCaches.translationMatrixWWs[meshIndex];
			rotationMatrixXXs[i] = g_meshProcessCaches.rotationMatrixXXs[meshIndex];
			rotationMatrixXYs[i] = g_meshProcessCaches.rotationMatrixXYs[meshIndex];
			rotationMatrixXZs[i] = g_meshProcessCaches.rotationMatrixXZs[meshIndex];
			rotationMatrixXWs[i] = g_meshProcessCaches.rotationMatrixXWs[meshIndex];
			rotationMatrixYXs[i] = g_meshProcessCaches.rotationMatrixYXs[meshIndex];
			rotationMatrixYYs[i] = g_meshProcessCaches.rotationMatrixYYs[meshIndex];
			rotationMatrixYZs[i] = g_meshProcessCaches.rotationMatrixYZs[meshIndex];
			rotationMatrixYWs[i] = g_meshProcessCaches.rotationMatrixYWs[meshIndex];
			rotationMatrixZXs[i] = g_meshProcessCaches.rotationMatrixZXs[meshIndex];
			rotationMatrixZYs[i] = g_meshProcessCaches.rotationMatrixZYs[meshIndex];
			rotationMatrixZZs[i] = g_meshProcessCaches.rotationMatrixZZs[meshIndex];
			rotationMatrixZWs[i] = g_meshProcessCaches.rotationMatrixZWs[meshIndex];
			rotationMatrixWXs[i] = g_meshProcessCaches.rotationMatrixWXs[meshIndex];
			rotationMatrixWYs[i] = g_meshProcessCaches.rotationMatrixWYs[meshIndex];
			rotationMatrixWZs[i] = g_meshProcessCaches.rotationMatrixWZs[meshIndex];
			rotationMatrixWWs[i] = g_meshProcessCaches.rotationMatrixWWs[meshIndex];
			scaleMatrixXXs[i] = g_meshProcessCaches.scaleMatrixXXs[meshIndex];
			scaleMatrixXYs[i] = g_meshProcessCaches.scaleMatrixXYs[meshIndex];
			scaleMatrixXZs[i] = g_meshProcessCaches.scaleMatrixXZs[meshIndex];
			scaleMatrixXWs[i] = g_meshProcessCaches.scaleMatrixXWs[meshIndex];
			scaleMatrixYXs[i] = g_meshProcessCaches.scaleMatrixYXs[meshIndex];
			scaleMatrixYYs[i] = g_meshProcessCaches.scaleMatrixYYs[meshIndex];
			scaleMatrixYZs[i] = g_meshProcessCaches.scaleMatrixYZs[meshIndex];
			scaleMatrixYWs[i] = g_meshProcessCaches.scaleMatrixYWs[meshIndex];
			scaleMatrixZXs[i] = g_meshProcessCaches.scaleMatrixZXs[meshIndex];
			scaleMatrixZYs[i] = g_meshProcessCaches.scaleMatrixZYs[meshIndex];
			scaleMatrixZZs[i] = g_meshProcessCaches.scaleMatrixZZs[meshIndex];
			scaleMatrixZWs[i] = g_meshProcessCaches.scaleMatrixZWs[meshIndex];
			scaleMatrixWXs[i] = g_meshProcessCaches.scaleMatrixWXs[meshIndex];
			scaleMatrixWYs[i] = g_meshProcessCaches.scaleMatrixWYs[meshIndex];
			scaleMatrixWZs[i] = g_meshProcessCaches.scaleMatrixWZs[meshIndex];
			scaleMatrixWWs[i] = g_meshProcessCaches.scaleMatrixWWs[meshIndex];
		}

		// Translate down so floor vertices go underground and ceiling is at y=0.
		const double preScaleTranslationWs[N] = { 0.0 };
		double vertexWithPreScaleTranslationXs[N];
		double vertexWithPreScaleTranslationYs[N];
		double vertexWithPreScaleTranslationZs[N];
		double vertexWithPreScaleTranslationWs[N];
		Double4_AddN<N>(vertexXs, vertexYs, vertexZs, vertexWs,
			preScaleTranslationXs, preScaleTranslationYs, preScaleTranslationZs, preScaleTranslationWs,
			vertexWithPreScaleTranslationXs, vertexWithPreScaleTranslationYs, vertexWithPreScaleTranslationZs, vertexWithPreScaleTranslationWs);

		// Shrink towards y=0 depending on anim percent and door min visible amount.
		double scaledVertexXs[N] = { 0.0 };
		double scaledVertexYs[N] = { 0.0 };
		double scaledVertexZs[N] = { 0.0 };
		double scaledVertexWs[N] = { 0.0 };
		Matrix4_MultiplyVectorN<N>(
			scaleMatrixXXs, scaleMatrixXYs, scaleMatrixXZs, scaleMatrixXWs,
			scaleMatrixYXs, scaleMatrixYYs, scaleMatrixYZs, scaleMatrixYWs,
			scaleMatrixZXs, scaleMatrixZYs, scaleMatrixZZs, scaleMatrixZWs,
			scaleMatrixWXs, scaleMatrixWYs, scaleMatrixWZs, scaleMatrixWWs,
			vertexWithPreScaleTranslationXs, vertexWithPreScaleTranslationYs, vertexWithPreScaleTranslationZs, vertexWithPreScaleTranslationWs,
			scaledVertexXs, scaledVertexYs, scaledVertexZs, scaledVertexWs);

		// Translate up to new model space Y position.
		double resultVertexXs[N];
		double resultVertexYs[N];
		double resultVertexZs[N];
		double resultVertexWs[N];
		Double4_SubtractN<N>(scaledVertexXs, scaledVertexYs, scaledVertexZs, scaledVertexWs,
			preScaleTranslationXs, preScaleTranslationYs, preScaleTranslationZs, preScaleTranslationWs,
			resultVertexXs, resultVertexYs, resultVertexZs, resultVertexWs);

		// Apply rotation matrix.
		double rotatedResultVertexXs[N] = { 0.0 };
		double rotatedResultVertexYs[N] = { 0.0 };
		double rotatedResultVertexZs[N] = { 0.0 };
		double rotatedResultVertexWs[N] = { 0.0 };
		Matrix4_MultiplyVectorN<N>(
			rotationMatrixXXs, rotationMatrixXYs, rotationMatrixXZs, rotationMatrixXWs,
			rotationMatrixYXs, rotationMatrixYYs, rotationMatrixYZs, rotationMatrixYWs,
			rotationMatrixZXs, rotationMatrixZYs, rotationMatrixZZs, rotationMatrixZWs,
			rotationMatrixWXs, rotationMatrixWYs, rotationMatrixWZs, rotationMatrixWWs,
			resultVertexXs, resultVertexYs, resultVertexZs, resultVertexWs,
			rotatedResultVertexXs, rotatedResultVertexYs, rotatedResultVertexZs, rotatedResultVertexWs);

		// Apply translation matrix.
		double translatedResultVertexXs[N] = { 0.0 };
		double translatedResultVertexYs[N] = { 0.0 };
		double translatedResultVertexZs[N] = { 0.0 };
		double translatedResultVertexWs[N] = { 0.0 };
		Matrix4_MultiplyVectorN<N>(
			translationMatrixXXs, translationMatrixXYs, translationMatrixXZs, translationMatrixXWs,
			translationMatrixYXs, translationMatrixYYs, translationMatrixYZs, translationMatrixYWs,
			translationMatrixZXs, translationMatrixZYs, translationMatrixZZs, translationMatrixZWs,
			translationMatrixWXs, translationMatrixWYs, translationMatrixWZs, translationMatrixWWs,
			rotatedResultVertexXs, rotatedResultVertexYs, rotatedResultVertexZs, rotatedResultVertexWs,
			translatedResultVertexXs, translatedResultVertexYs, translatedResultVertexZs, translatedResultVertexWs);

		// Apply view-projection matrix.
		Matrix4_MultiplyVectorN<N>(
			g_viewProjMatrixXX, g_viewProjMatrixXY, g_viewProjMatrixXZ, g_viewProjMatrixXW,
			g_viewProjMatrixYX, g_viewProjMatrixYY, g_viewProjMatrixYZ, g_viewProjMatrixYW,
			g_viewProjMatrixZX, g_viewProjMatrixZY, g_viewProjMatrixZZ, g_viewProjMatrixZW,
			g_viewProjMatrixWX, g_viewProjMatrixWY, g_viewProjMatrixWZ, g_viewProjMatrixWW,
			translatedResultVertexXs, translatedResultVertexYs, translatedResultVertexZs, translatedResultVertexWs,
			outVertexXs, outVertexYs, outVertexZs, outVertexWs);
	}

	template<int N>
	void VertexShader_EntityN(const int *__restrict meshIndices, const double *__restrict vertexXs, const double *__restrict vertexYs,
		const double *__restrict vertexZs, const double *__restrict vertexWs, double *__restrict outVertexXs, double *__restrict outVertexYs,
		double *__restrict outVertexZs, double *__restrict outVertexWs)
	{
		double modelViewProjMatrixXXs[N];
		double modelViewProjMatrixXYs[N];
		double modelViewProjMatrixXZs[N];
		double modelViewProjMatrixXWs[N];
		double modelViewProjMatrixYXs[N];
		double modelViewProjMatrixYYs[N];
		double modelViewProjMatrixYZs[N];
		double modelViewProjMatrixYWs[N];
		double modelViewProjMatrixZXs[N];
		double modelViewProjMatrixZYs[N];
		double modelViewProjMatrixZZs[N];
		double modelViewProjMatrixZWs[N];
		double modelViewProjMatrixWXs[N];
		double modelViewProjMatrixWYs[N];
		double modelViewProjMatrixWZs[N];
		double modelViewProjMatrixWWs[N];
		for (int i = 0; i < N; i++)
		{
			const int meshIndex = meshIndices[i];
			modelViewProjMatrixXXs[i] = g_meshProcessCaches.modelViewProjMatrixXXs[meshIndex];
			modelViewProjMatrixXYs[i] = g_meshProcessCaches.modelViewProjMatrixXYs[meshIndex];
			modelViewProjMatrixXZs[i] = g_meshProcessCaches.modelViewProjMatrixXZs[meshIndex];
			modelViewProjMatrixXWs[i] = g_meshProcessCaches.modelViewProjMatrixXWs[meshIndex];
			modelViewProjMatrixYXs[i] = g_meshProcessCaches.modelViewProjMatrixYXs[meshIndex];
			modelViewProjMatrixYYs[i] = g_meshProcessCaches.modelViewProjMatrixYYs[meshIndex];
			modelViewProjMatrixYZs[i] = g_meshProcessCaches.modelViewProjMatrixYZs[meshIndex];
			modelViewProjMatrixYWs[i] = g_meshProcessCaches.modelViewProjMatrixYWs[meshIndex];
			modelViewProjMatrixZXs[i] = g_meshProcessCaches.modelViewProjMatrixZXs[meshIndex];
			modelViewProjMatrixZYs[i] = g_meshProcessCaches.modelViewProjMatrixZYs[meshIndex];
			modelViewProjMatrixZZs[i] = g_meshProcessCaches.modelViewProjMatrixZZs[meshIndex];
			modelViewProjMatrixZWs[i] = g_meshProcessCaches.modelViewProjMatrixZWs[meshIndex];
			modelViewProjMatrixWXs[i] = g_meshProcessCaches.modelViewProjMatrixWXs[meshIndex];
			modelViewProjMatrixWYs[i] = g_meshProcessCaches.modelViewProjMatrixWYs[meshIndex];
			modelViewProjMatrixWZs[i] = g_meshProcessCaches.modelViewProjMatrixWZs[meshIndex];
			modelViewProjMatrixWWs[i] = g_meshProcessCaches.modelViewProjMatrixWWs[meshIndex];
		}

		// Apply model-view-projection matrix.
		Matrix4_MultiplyVectorN<N>(
			modelViewProjMatrixXXs, modelViewProjMatrixXYs, modelViewProjMatrixXZs, modelViewProjMatrixXWs,
			modelViewProjMatrixYXs, modelViewProjMatrixYYs, modelViewProjMatrixYZs, modelViewProjMatrixYWs,
			modelViewProjMatrixZXs, modelViewProjMatrixZYs, modelViewProjMatrixZZs, modelViewProjMatrixZWs,
			modelViewProjMatrixWXs, modelViewProjMatrixWYs, modelViewProjMatrixWZs, modelViewProjMatrixWWs,
			vertexXs, vertexYs, vertexZs, vertexWs,
			outVertexXs, outVertexYs, outVertexZs, outVertexWs);
	}
}

// Pixel shaders.
namespace
{
	struct PixelShaderPerspectiveCorrection
	{
		double ndcZDepth;
		double texelPercentX;
		double texelPercentY;
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
		// Based on camera forward direction as XZ vector.
		double horizonScreenSpacePointX;
		double horizonScreenSpacePointY;

		int reflectedPixelIndex;
		bool isReflectedPixelInFrameBuffer;
		uint8_t fallbackSkyColor;
	};

	struct PixelShaderFrameBuffer
	{
		PixelShaderPalette palette;
		double xPercent, yPercent;
		int pixelIndex;
	};

	double g_ambientPercent;
	const SoftwareRenderer::ObjectTexture *g_paletteTexture; // 8-bit -> 32-bit color conversion palette.
	const SoftwareRenderer::ObjectTexture *g_lightTableTexture; // Shading/transparency look-ups.
	const SoftwareRenderer::ObjectTexture *g_skyBgTexture; // Fallback sky texture for horizon reflection shader.

	void PopulatePixelShaderGlobals(double ambientPercent, const SoftwareRenderer::ObjectTexture &paletteTexture,
		const SoftwareRenderer::ObjectTexture &lightTableTexture, const SoftwareRenderer::ObjectTexture &skyBgTexture)
	{
		g_ambientPercent = ambientPercent;
		g_paletteTexture = &paletteTexture;
		g_lightTableTexture = &lightTableTexture;
		g_skyBgTexture = &skyBgTexture;
	}

	template<bool enableDepthWrite>
	void PixelShader_Opaque(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		int texelX = -1;
		int texelY = -1;
		if (texture.samplingType == TextureSamplingType::Default)
		{
			texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
			texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
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
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = shadedTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_OpaqueWithAlphaTestLayer(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &opaqueTexture,
		const PixelShaderTexture &alphaTestTexture, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int layerTexelX = std::clamp(static_cast<int>(perspective.texelPercentX * alphaTestTexture.widthReal), 0, alphaTestTexture.widthMinusOne);
		const int layerTexelY = std::clamp(static_cast<int>(perspective.texelPercentY * alphaTestTexture.heightReal), 0, alphaTestTexture.heightMinusOne);
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
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = shadedTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTested(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = shadedTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithVariableTexCoordUMin(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		double uMin, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const double u = std::clamp(uMin + ((1.0 - uMin) * perspective.texelPercentX), uMin, 1.0);
		const int texelX = std::clamp(static_cast<int>(u * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.height), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int shadedTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t shadedTexel = lighting.lightTableTexels[shadedTexelIndex];
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = shadedTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithVariableTexCoordVMin(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		double vMin, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const double v = std::clamp(vMin + ((1.0 - vMin) * perspective.texelPercentY), vMin, 1.0);
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
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = shadedTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithPaletteIndexLookup(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderTexture &lookupTexture, const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
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
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = shadedTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithLightLevelColor(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		const int lightTableTexelIndex = texel + (lighting.lightLevel * lighting.texelsPerLightLevel);
		const uint8_t resultTexel = lighting.lightTableTexels[lightTableTexelIndex];

		g_paletteIndexBuffer[frameBuffer.pixelIndex] = resultTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithLightLevelOpacity(const PixelShaderPerspectiveCorrection &perspective, const PixelShaderTexture &texture,
		const PixelShaderLighting &lighting, PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
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
			const uint8_t prevFrameBufferPixel = g_paletteIndexBuffer[frameBuffer.pixelIndex];
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
		g_paletteIndexBuffer[frameBuffer.pixelIndex] = resultTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithPreviousBrightnessLimit(const PixelShaderPerspectiveCorrection &perspective,
		const PixelShaderTexture &texture, PixelShaderFrameBuffer &frameBuffer)
	{
		constexpr int brightnessLimit = 0x3F; // Highest value each RGB component can be.
		constexpr uint8_t brightnessMask = ~brightnessLimit;
		constexpr uint32_t brightnessMaskR = brightnessMask << 16;
		constexpr uint32_t brightnessMaskG = brightnessMask << 8;
		constexpr uint32_t brightnessMaskB = brightnessMask;
		constexpr uint32_t brightnessMaskRGB = brightnessMaskR | brightnessMaskG | brightnessMaskB;

		const uint8_t prevFrameBufferPixel = g_paletteIndexBuffer[frameBuffer.pixelIndex];
		const uint32_t prevFrameBufferColor = frameBuffer.palette.colors[prevFrameBufferPixel];
		const bool isDarkEnough = (prevFrameBufferColor & brightnessMaskRGB) == 0;
		if (!isDarkEnough)
		{
			return;
		}

		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
		const int texelIndex = texelX + (texelY * texture.width);
		const uint8_t texel = texture.texels[texelIndex];

		const bool isTransparent = texel == ArenaRenderUtils::PALETTE_INDEX_TRANSPARENT;
		if (isTransparent)
		{
			return;
		}

		g_paletteIndexBuffer[frameBuffer.pixelIndex] = texel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}

	template<bool enableDepthWrite>
	void PixelShader_AlphaTestedWithHorizonMirror(const PixelShaderPerspectiveCorrection &perspective,
		const PixelShaderTexture &texture, const PixelShaderHorizonMirror &horizon, const PixelShaderLighting &lighting,
		PixelShaderFrameBuffer &frameBuffer)
	{
		const int texelX = std::clamp(static_cast<int>(perspective.texelPercentX * texture.widthReal), 0, texture.widthMinusOne);
		const int texelY = std::clamp(static_cast<int>(perspective.texelPercentY * texture.heightReal), 0, texture.heightMinusOne);
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
				const uint8_t mirroredTexel = g_paletteIndexBuffer[horizon.reflectedPixelIndex];
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

		g_paletteIndexBuffer[frameBuffer.pixelIndex] = resultTexel;

		if constexpr (enableDepthWrite)
		{
			g_depthBuffer[frameBuffer.pixelIndex] = perspective.ndcZDepth;
		}
	}
}

// Mesh processing, vertex shader execution.
namespace
{
	// One per group of mesh process caches, for improving number crunching efficiency with vertex shading by
	// keeping the triangle count much higher than the average 2 per draw call.
	struct VertexShadingCache
	{
		double unshadedV0Xs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV0Ys[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV0Zs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV0Ws[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV1Xs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV1Ys[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV1Zs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV1Ws[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV2Xs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV2Ys[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV2Zs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double unshadedV2Ws[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double uv0Xs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double uv0Ys[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double uv1Xs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double uv1Ys[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double uv2Xs[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		double uv2Ys[MAX_VERTEX_SHADING_CACHE_TRIANGLES];
		int meshProcessCacheIndices[MAX_VERTEX_SHADING_CACHE_TRIANGLES]; // Each triangle's mesh process cache it belongs to.
		int triangleCount;
	};

	VertexShadingCache g_vertexShadingCache;

	int g_totalDrawCallCount = 0;
	int g_totalPresentedTriangleCount = 0; // Triangles the rasterizer spends any time attempting to shade pixels for.

	void ClearTriangleTotalCounts()
	{
		// Skip zeroing mesh process caches for performance.
		g_totalPresentedTriangleCount = 0;
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
			const int meshTriangleCount = indexBuffer.triangleCount;
			DebugAssert(meshTriangleCount <= MAX_DRAW_CALL_MESH_TRIANGLES);

			int writeIndex = g_vertexShadingCache.triangleCount;
			DebugAssert((writeIndex + meshTriangleCount) <= MAX_VERTEX_SHADING_CACHE_TRIANGLES);
			for (int triangleIndex = 0; triangleIndex < meshTriangleCount; triangleIndex++)
			{
				constexpr int indicesPerTriangle = 3;
				constexpr int positionComponentsPerVertex = 3;
				constexpr int texCoordComponentsPerVertex = 2;
				const int indexBufferBase = triangleIndex * indicesPerTriangle;
				const int32_t index0 = indicesPtr[indexBufferBase];
				const int32_t index1 = indicesPtr[indexBufferBase + 1];
				const int32_t index2 = indicesPtr[indexBufferBase + 2];
				const int32_t v0Index = index0 * positionComponentsPerVertex;
				const int32_t v1Index = index1 * positionComponentsPerVertex;
				const int32_t v2Index = index2 * positionComponentsPerVertex;
				const int32_t uv0Index = index0 * texCoordComponentsPerVertex;
				const int32_t uv1Index = index1 * texCoordComponentsPerVertex;
				const int32_t uv2Index = index2 * texCoordComponentsPerVertex;
				g_vertexShadingCache.unshadedV0Xs[writeIndex] = verticesPtr[v0Index];
				g_vertexShadingCache.unshadedV0Ys[writeIndex] = verticesPtr[v0Index + 1];
				g_vertexShadingCache.unshadedV0Zs[writeIndex] = verticesPtr[v0Index + 2];
				g_vertexShadingCache.unshadedV0Ws[writeIndex] = 1.0;
				g_vertexShadingCache.unshadedV1Xs[writeIndex] = verticesPtr[v1Index];
				g_vertexShadingCache.unshadedV1Ys[writeIndex] = verticesPtr[v1Index + 1];
				g_vertexShadingCache.unshadedV1Zs[writeIndex] = verticesPtr[v1Index + 2];
				g_vertexShadingCache.unshadedV1Ws[writeIndex] = 1.0;
				g_vertexShadingCache.unshadedV2Xs[writeIndex] = verticesPtr[v2Index];
				g_vertexShadingCache.unshadedV2Ys[writeIndex] = verticesPtr[v2Index + 1];
				g_vertexShadingCache.unshadedV2Zs[writeIndex] = verticesPtr[v2Index + 2];
				g_vertexShadingCache.unshadedV2Ws[writeIndex] = 1.0;
				g_vertexShadingCache.uv0Xs[writeIndex] = texCoordsPtr[uv0Index];
				g_vertexShadingCache.uv0Ys[writeIndex] = texCoordsPtr[uv0Index + 1];
				g_vertexShadingCache.uv1Xs[writeIndex] = texCoordsPtr[uv1Index];
				g_vertexShadingCache.uv1Ys[writeIndex] = texCoordsPtr[uv1Index + 1];
				g_vertexShadingCache.uv2Xs[writeIndex] = texCoordsPtr[uv2Index];
				g_vertexShadingCache.uv2Ys[writeIndex] = texCoordsPtr[uv2Index + 1];
				g_vertexShadingCache.meshProcessCacheIndices[writeIndex] = meshIndex;
				writeIndex++;
			}

			g_vertexShadingCache.triangleCount += meshTriangleCount;
		}
	}

	void CalculateVertexShaderTransforms(int meshCount)
	{
		constexpr int loopUnrollCount = TYPICAL_LOOP_UNROLL;
		static_assert(loopUnrollCount <= MAX_MESH_PROCESS_CACHES);

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

		const int meshCountUnrollAdjusted = GetUnrollAdjustedLoopCount(meshCount, loopUnrollCount);
		int meshIndex = 0;
		while (meshIndex < meshCountUnrollAdjusted)
		{
			// Rotation-scale matrix
			Matrix4_MultiplyMatrixN<loopUnrollCount>(
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
			Matrix4_MultiplyMatrixN<loopUnrollCount>(
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
			Matrix4_MultiplyMatrixN<loopUnrollCount>(
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

			meshIndex += loopUnrollCount;
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

		constexpr int loopUnrollCount = TYPICAL_LOOP_UNROLL;
		static_assert(loopUnrollCount <= MAX_MESH_PROCESS_CACHES);

		// Run vertex shaders on each triangle and store the results for clipping.
		const int triangleCount = g_vertexShadingCache.triangleCount;
		const int triangleCountUnrollAdjusted = GetUnrollAdjustedLoopCount(triangleCount, loopUnrollCount);
		int triangleIndex = 0;
		while (triangleIndex < triangleCountUnrollAdjusted)
		{
			const int *meshIndices = g_vertexShadingCache.meshProcessCacheIndices + triangleIndex;
			const double *unshadedV0Xs = g_vertexShadingCache.unshadedV0Xs + triangleIndex;
			const double *unshadedV0Ys = g_vertexShadingCache.unshadedV0Ys + triangleIndex;
			const double *unshadedV0Zs = g_vertexShadingCache.unshadedV0Zs + triangleIndex;
			const double *unshadedV0Ws = g_vertexShadingCache.unshadedV0Ws + triangleIndex;
			const double *unshadedV1Xs = g_vertexShadingCache.unshadedV1Xs + triangleIndex;
			const double *unshadedV1Ys = g_vertexShadingCache.unshadedV1Ys + triangleIndex;
			const double *unshadedV1Zs = g_vertexShadingCache.unshadedV1Zs + triangleIndex;
			const double *unshadedV1Ws = g_vertexShadingCache.unshadedV1Ws + triangleIndex;
			const double *unshadedV2Xs = g_vertexShadingCache.unshadedV2Xs + triangleIndex;
			const double *unshadedV2Ys = g_vertexShadingCache.unshadedV2Ys + triangleIndex;
			const double *unshadedV2Zs = g_vertexShadingCache.unshadedV2Zs + triangleIndex;
			const double *unshadedV2Ws = g_vertexShadingCache.unshadedV2Ws + triangleIndex;
			double shadedV0Xs[loopUnrollCount] = { 0.0 };
			double shadedV0Ys[loopUnrollCount] = { 0.0 };
			double shadedV0Zs[loopUnrollCount] = { 0.0 };
			double shadedV0Ws[loopUnrollCount] = { 0.0 };
			double shadedV1Xs[loopUnrollCount] = { 0.0 };
			double shadedV1Ys[loopUnrollCount] = { 0.0 };
			double shadedV1Zs[loopUnrollCount] = { 0.0 };
			double shadedV1Ws[loopUnrollCount] = { 0.0 };
			double shadedV2Xs[loopUnrollCount] = { 0.0 };
			double shadedV2Ys[loopUnrollCount] = { 0.0 };
			double shadedV2Zs[loopUnrollCount] = { 0.0 };
			double shadedV2Ws[loopUnrollCount] = { 0.0 };

			if constexpr (vertexShaderType == VertexShaderType::Basic)
			{
				VertexShader_BasicN<loopUnrollCount>(meshIndices, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_BasicN<loopUnrollCount>(meshIndices, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_BasicN<loopUnrollCount>(meshIndices, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}
			else if (vertexShaderType == VertexShaderType::RaisingDoor)
			{
				VertexShader_RaisingDoorN<loopUnrollCount>(meshIndices, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_RaisingDoorN<loopUnrollCount>(meshIndices, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_RaisingDoorN<loopUnrollCount>(meshIndices, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}
			else if (vertexShaderType == VertexShaderType::Entity)
			{
				VertexShader_EntityN<loopUnrollCount>(meshIndices, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_EntityN<loopUnrollCount>(meshIndices, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_EntityN<loopUnrollCount>(meshIndices, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}

			for (int i = 0; i < loopUnrollCount; i++)
			{
				const int unrollMeshIndex = meshIndices[i];
				int &writeIndex = g_meshProcessCaches.triangleWriteCounts[unrollMeshIndex];
				DebugAssert(writeIndex < MAX_DRAW_CALL_MESH_TRIANGLES);

				// Change from SoA back to AoS due to how clipping currently reads vertices.
				// - @todo: understand how to make clipping use SoA so the shadedV0/1/2 array can just go straight to these instead
				auto &resultV0XYZW = g_meshProcessCaches.shadedV0XYZWArrays[unrollMeshIndex][writeIndex];
				auto &resultV1XYZW = g_meshProcessCaches.shadedV1XYZWArrays[unrollMeshIndex][writeIndex];
				auto &resultV2XYZW = g_meshProcessCaches.shadedV2XYZWArrays[unrollMeshIndex][writeIndex];
				auto &resultUV0XY = g_meshProcessCaches.uv0XYArrays[unrollMeshIndex][writeIndex];
				auto &resultUV1XY = g_meshProcessCaches.uv1XYArrays[unrollMeshIndex][writeIndex];
				auto &resultUV2XY = g_meshProcessCaches.uv2XYArrays[unrollMeshIndex][writeIndex];
				resultV0XYZW[0] = shadedV0Xs[i];
				resultV0XYZW[1] = shadedV0Ys[i];
				resultV0XYZW[2] = shadedV0Zs[i];
				resultV0XYZW[3] = shadedV0Ws[i];
				resultV1XYZW[0] = shadedV1Xs[i];
				resultV1XYZW[1] = shadedV1Ys[i];
				resultV1XYZW[2] = shadedV1Zs[i];
				resultV1XYZW[3] = shadedV1Ws[i];
				resultV2XYZW[0] = shadedV2Xs[i];
				resultV2XYZW[1] = shadedV2Ys[i];
				resultV2XYZW[2] = shadedV2Zs[i];
				resultV2XYZW[3] = shadedV2Ws[i];

				const int unrollTriangleIndex = triangleIndex + i;
				resultUV0XY[0] = g_vertexShadingCache.uv0Xs[unrollTriangleIndex];
				resultUV0XY[1] = g_vertexShadingCache.uv0Ys[unrollTriangleIndex];
				resultUV1XY[0] = g_vertexShadingCache.uv1Xs[unrollTriangleIndex];
				resultUV1XY[1] = g_vertexShadingCache.uv1Ys[unrollTriangleIndex];
				resultUV2XY[0] = g_vertexShadingCache.uv2Xs[unrollTriangleIndex];
				resultUV2XY[1] = g_vertexShadingCache.uv2Ys[unrollTriangleIndex];
				writeIndex++;
			}

			triangleIndex += loopUnrollCount;
		}

		while (triangleIndex < triangleCount)
		{
			const int meshIndex = g_vertexShadingCache.meshProcessCacheIndices[triangleIndex];
			const double unshadedV0Xs[1] = { g_vertexShadingCache.unshadedV0Xs[triangleIndex] };
			const double unshadedV0Ys[1] = { g_vertexShadingCache.unshadedV0Ys[triangleIndex] };
			const double unshadedV0Zs[1] = { g_vertexShadingCache.unshadedV0Zs[triangleIndex] };
			const double unshadedV0Ws[1] = { g_vertexShadingCache.unshadedV0Ws[triangleIndex] };
			const double unshadedV1Xs[1] = { g_vertexShadingCache.unshadedV1Xs[triangleIndex] };
			const double unshadedV1Ys[1] = { g_vertexShadingCache.unshadedV1Ys[triangleIndex] };
			const double unshadedV1Zs[1] = { g_vertexShadingCache.unshadedV1Zs[triangleIndex] };
			const double unshadedV1Ws[1] = { g_vertexShadingCache.unshadedV1Ws[triangleIndex] };
			const double unshadedV2Xs[1] = { g_vertexShadingCache.unshadedV2Xs[triangleIndex] };
			const double unshadedV2Ys[1] = { g_vertexShadingCache.unshadedV2Ys[triangleIndex] };
			const double unshadedV2Zs[1] = { g_vertexShadingCache.unshadedV2Zs[triangleIndex] };
			const double unshadedV2Ws[1] = { g_vertexShadingCache.unshadedV2Ws[triangleIndex] };
			double shadedV0Xs[1] = { 0.0 };
			double shadedV0Ys[1] = { 0.0 };
			double shadedV0Zs[1] = { 0.0 };
			double shadedV0Ws[1] = { 0.0 };
			double shadedV1Xs[1] = { 0.0 };
			double shadedV1Ys[1] = { 0.0 };
			double shadedV1Zs[1] = { 0.0 };
			double shadedV1Ws[1] = { 0.0 };
			double shadedV2Xs[1] = { 0.0 };
			double shadedV2Ys[1] = { 0.0 };
			double shadedV2Zs[1] = { 0.0 };
			double shadedV2Ws[1] = { 0.0 };

			if constexpr (vertexShaderType == VertexShaderType::Basic)
			{
				VertexShader_BasicN<1>(&meshIndex, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_BasicN<1>(&meshIndex, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_BasicN<1>(&meshIndex, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}
			else if (vertexShaderType == VertexShaderType::RaisingDoor)
			{
				VertexShader_RaisingDoorN<1>(&meshIndex, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_RaisingDoorN<1>(&meshIndex, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_RaisingDoorN<1>(&meshIndex, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}
			else if (vertexShaderType == VertexShaderType::Entity)
			{
				VertexShader_EntityN<1>(&meshIndex, unshadedV0Xs, unshadedV0Ys, unshadedV0Zs, unshadedV0Ws, shadedV0Xs, shadedV0Ys, shadedV0Zs, shadedV0Ws);
				VertexShader_EntityN<1>(&meshIndex, unshadedV1Xs, unshadedV1Ys, unshadedV1Zs, unshadedV1Ws, shadedV1Xs, shadedV1Ys, shadedV1Zs, shadedV1Ws);
				VertexShader_EntityN<1>(&meshIndex, unshadedV2Xs, unshadedV2Ys, unshadedV2Zs, unshadedV2Ws, shadedV2Xs, shadedV2Ys, shadedV2Zs, shadedV2Ws);
			}

			int &writeIndex = g_meshProcessCaches.triangleWriteCounts[meshIndex];
			DebugAssert(writeIndex < MAX_DRAW_CALL_MESH_TRIANGLES);

			auto &resultV0XYZW = g_meshProcessCaches.shadedV0XYZWArrays[meshIndex][writeIndex];
			auto &resultV1XYZW = g_meshProcessCaches.shadedV1XYZWArrays[meshIndex][writeIndex];
			auto &resultV2XYZW = g_meshProcessCaches.shadedV2XYZWArrays[meshIndex][writeIndex];
			auto &resultUV0XY = g_meshProcessCaches.uv0XYArrays[meshIndex][writeIndex];
			auto &resultUV1XY = g_meshProcessCaches.uv1XYArrays[meshIndex][writeIndex];
			auto &resultUV2XY = g_meshProcessCaches.uv2XYArrays[meshIndex][writeIndex];
			resultV0XYZW[0] = shadedV0Xs[0];
			resultV0XYZW[1] = shadedV0Ys[0];
			resultV0XYZW[2] = shadedV0Zs[0];
			resultV0XYZW[3] = shadedV0Ws[0];
			resultV1XYZW[0] = shadedV1Xs[0];
			resultV1XYZW[1] = shadedV1Ys[0];
			resultV1XYZW[2] = shadedV1Zs[0];
			resultV1XYZW[3] = shadedV1Ws[0];
			resultV2XYZW[0] = shadedV2Xs[0];
			resultV2XYZW[1] = shadedV2Ys[0];
			resultV2XYZW[2] = shadedV2Zs[0];
			resultV2XYZW[3] = shadedV2Ws[0];
			resultUV0XY[0] = g_vertexShadingCache.uv0Xs[triangleIndex];
			resultUV0XY[1] = g_vertexShadingCache.uv0Ys[triangleIndex];
			resultUV1XY[0] = g_vertexShadingCache.uv1Xs[triangleIndex];
			resultUV1XY[1] = g_vertexShadingCache.uv1Ys[triangleIndex];
			resultUV2XY[0] = g_vertexShadingCache.uv2Xs[triangleIndex];
			resultUV2XY[1] = g_vertexShadingCache.uv2Ys[triangleIndex];
			writeIndex++;
			triangleIndex++;
		}
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

	template<int clipPlaneIndex>
	void ProcessClippingWithPlane(int meshIndex, int &clipListSize, int &clipListFrontIndex)
	{
		auto &clipSpaceTriangleV0XYZWs = g_meshProcessCaches.clipSpaceTriangleV0XYZWArrays[meshIndex];
		auto &clipSpaceTriangleV1XYZWs = g_meshProcessCaches.clipSpaceTriangleV1XYZWArrays[meshIndex];
		auto &clipSpaceTriangleV2XYZWs = g_meshProcessCaches.clipSpaceTriangleV2XYZWArrays[meshIndex];
		auto &clipSpaceTriangleUV0XYs = g_meshProcessCaches.clipSpaceTriangleUV0XYArrays[meshIndex];
		auto &clipSpaceTriangleUV1XYs = g_meshProcessCaches.clipSpaceTriangleUV1XYArrays[meshIndex];
		auto &clipSpaceTriangleUV2XYs = g_meshProcessCaches.clipSpaceTriangleUV2XYArrays[meshIndex];

		const int trianglesToClipCount = clipListSize - clipListFrontIndex;
		for (int triangleToClip = trianglesToClipCount; triangleToClip > 0; triangleToClip--)
		{
			const auto &clipSpaceTriangleV0XYZW = clipSpaceTriangleV0XYZWs[clipListFrontIndex];
			const auto &clipSpaceTriangleV1XYZW = clipSpaceTriangleV1XYZWs[clipListFrontIndex];
			const auto &clipSpaceTriangleV2XYZW = clipSpaceTriangleV2XYZWs[clipListFrontIndex];
			const auto &clipSpaceTriangleUV0XY = clipSpaceTriangleUV0XYs[clipListFrontIndex];
			const auto &clipSpaceTriangleUV1XY = clipSpaceTriangleUV1XYs[clipListFrontIndex];
			const auto &clipSpaceTriangleUV2XY = clipSpaceTriangleUV2XYs[clipListFrontIndex];

			// Active vertices for clipping. The last two are populated below if clipping is needed.
			double currentVXs[] = { clipSpaceTriangleV0XYZW[0], clipSpaceTriangleV1XYZW[0], clipSpaceTriangleV2XYZW[0], 0.0, 0.0 };
			double currentVYs[] = { clipSpaceTriangleV0XYZW[1], clipSpaceTriangleV1XYZW[1], clipSpaceTriangleV2XYZW[1], 0.0, 0.0 };
			double currentVZs[] = { clipSpaceTriangleV0XYZW[2], clipSpaceTriangleV1XYZW[2], clipSpaceTriangleV2XYZW[2], 0.0, 0.0 };
			double currentVWs[] = { clipSpaceTriangleV0XYZW[3], clipSpaceTriangleV1XYZW[3], clipSpaceTriangleV2XYZW[3], 0.0, 0.0 };
			constexpr int GENERATED_RESULT_INDEX0 = 3;
			constexpr int GENERATED_RESULT_INDEX1 = 4;

			double v0Component, v1Component, v2Component;
			if constexpr ((clipPlaneIndex == 0) || (clipPlaneIndex == 1))
			{
				v0Component = currentVXs[0];
				v1Component = currentVXs[1];
				v2Component = currentVXs[2];
			}
			else if ((clipPlaneIndex == 2) || (clipPlaneIndex == 3))
			{
				v0Component = currentVYs[0];
				v1Component = currentVYs[1];
				v2Component = currentVYs[2];
			}
			else
			{
				v0Component = currentVZs[0];
				v1Component = currentVZs[1];
				v2Component = currentVZs[2];
			}

			double v0w, v1w, v2w;
			double comparisonSign;
			if constexpr ((clipPlaneIndex & 1) == 0)
			{
				v0w = currentVWs[0];
				v1w = currentVWs[1];
				v2w = currentVWs[2];
				comparisonSign = 1.0;
			}
			else
			{
				v0w = -currentVWs[0];
				v1w = -currentVWs[1];
				v2w = -currentVWs[2];
				comparisonSign = -1.0;
			}

			const double vDiffs[] = { v0Component + v0w, v1Component + v1w, v2Component + v2w };
			const bool isV0Inside = (vDiffs[0] * comparisonSign) >= 0.0;
			const bool isV1Inside = (vDiffs[1] * comparisonSign) >= 0.0;
			const bool isV2Inside = (vDiffs[2] * comparisonSign) >= 0.0;

			// Active texture coordinates for clipping, same rule as vertices above.
			double currentUVXs[] = { clipSpaceTriangleUV0XY[0], clipSpaceTriangleUV1XY[0], clipSpaceTriangleUV2XY[0], 0.0, 0.0 };
			double currentUVYs[] = { clipSpaceTriangleUV0XY[1], clipSpaceTriangleUV1XY[1], clipSpaceTriangleUV2XY[1], 0.0, 0.0 };

			const int resultWriteIndex0 = clipListSize;
			const int resultWriteIndex1 = clipListSize + 1;
			auto &result0V0XYZW = clipSpaceTriangleV0XYZWs[resultWriteIndex0];
			auto &result0V1XYZW = clipSpaceTriangleV1XYZWs[resultWriteIndex0];
			auto &result0V2XYZW = clipSpaceTriangleV2XYZWs[resultWriteIndex0];
			auto &result1V0XYZW = clipSpaceTriangleV0XYZWs[resultWriteIndex1];
			auto &result1V1XYZW = clipSpaceTriangleV1XYZWs[resultWriteIndex1];
			auto &result1V2XYZW = clipSpaceTriangleV2XYZWs[resultWriteIndex1];
			auto &result0UV0XY = clipSpaceTriangleUV0XYs[resultWriteIndex0];
			auto &result0UV1XY = clipSpaceTriangleUV1XYs[resultWriteIndex0];
			auto &result0UV2XY = clipSpaceTriangleUV2XYs[resultWriteIndex0];
			auto &result1UV0XY = clipSpaceTriangleUV0XYs[resultWriteIndex1];
			auto &result1UV1XY = clipSpaceTriangleUV1XYs[resultWriteIndex1];
			auto &result1UV2XY = clipSpaceTriangleUV2XYs[resultWriteIndex1];

			const int insideMaskIndex = (isV2Inside ? 0 : 1) | (isV1Inside ? 0 : 2) | (isV0Inside ? 0 : 4);
			constexpr int clipCaseResultTriangleCounts[] =
			{
				1, // All three input vertices visible
				2, // Becomes quad (Inside: V0, V1. Outside: V2)
				2, // Becomes quad (Inside: V0, V2. Outside: V1)
				1, // Becomes smaller triangle (Inside: V0. Outside: V1, V2)
				2, // Becomes quad (Inside: V1, V2. Outside: V0)
				1, // Becomes smaller triangle (Inside: V1. Outside: V0, V2)
				1, // Becomes smaller triangle (Inside: V2. Outside: V0, V1)
				0 // No input vertices visible
			};

			const int clipResultCount = clipCaseResultTriangleCounts[insideMaskIndex];
			const bool becomesQuad = clipResultCount == 2;

			if (insideMaskIndex == 0)
			{
				// All vertices visible, no clipping needed.
				result0V0XYZW[0] = currentVXs[0];
				result0V0XYZW[1] = currentVYs[0];
				result0V0XYZW[2] = currentVZs[0];
				result0V0XYZW[3] = currentVWs[0];
				result0V1XYZW[0] = currentVXs[1];
				result0V1XYZW[1] = currentVYs[1];
				result0V1XYZW[2] = currentVZs[1];
				result0V1XYZW[3] = currentVWs[1];
				result0V2XYZW[0] = currentVXs[2];
				result0V2XYZW[1] = currentVYs[2];
				result0V2XYZW[2] = currentVZs[2];
				result0V2XYZW[3] = currentVWs[2];
				result0UV0XY[0] = currentUVXs[0];
				result0UV0XY[1] = currentUVYs[0];
				result0UV1XY[0] = currentUVXs[1];
				result0UV1XY[1] = currentUVYs[1];
				result0UV2XY[0] = currentUVXs[2];
				result0UV2XY[1] = currentUVYs[2];
			}
			else if (insideMaskIndex == 7)
			{
				// All three vertices outside frustum, write nothing.
			}
			else
			{
				// Determine which two line segments are intersecting the clipping plane. The input and result
				// vertex orders depend on the clip case.
				int inputIndex0, inputIndex1, inputIndex2, inputIndex3;
				int resultIndex0, resultIndex1, resultIndex2, resultIndex3, resultIndex4, resultIndex5;
				switch (insideMaskIndex)
				{
				case 1:
					inputIndex0 = 1;
					inputIndex1 = 2;
					inputIndex2 = 2;
					inputIndex3 = 0;
					resultIndex0 = 0;
					resultIndex1 = 1;
					resultIndex2 = 3;
					resultIndex3 = 3;
					resultIndex4 = 4;
					resultIndex5 = 0;
					break;
				case 2:
					inputIndex0 = 0;
					inputIndex1 = 1;
					inputIndex2 = 1;
					inputIndex3 = 2;
					resultIndex0 = 0;
					resultIndex1 = 3;
					resultIndex2 = 4;
					resultIndex3 = 4;
					resultIndex4 = 2;
					resultIndex5 = 0;
					break;
				case 3:
					inputIndex0 = 0;
					inputIndex1 = 1;
					inputIndex2 = 2;
					inputIndex3 = 0;
					resultIndex0 = 0;
					resultIndex1 = 3;
					resultIndex2 = 4;
					break;
				case 4:
					inputIndex0 = 0;
					inputIndex1 = 1;
					inputIndex2 = 2;
					inputIndex3 = 0;
					resultIndex0 = 3;
					resultIndex1 = 1;
					resultIndex2 = 2;
					resultIndex3 = 2;
					resultIndex4 = 4;
					resultIndex5 = 3;
					break;
				case 5:
					inputIndex0 = 0;
					inputIndex1 = 1;
					inputIndex2 = 1;
					inputIndex3 = 2;
					resultIndex0 = 3;
					resultIndex1 = 1;
					resultIndex2 = 4;
					break;
				case 6:
					inputIndex0 = 1;
					inputIndex1 = 2;
					inputIndex2 = 2;
					inputIndex3 = 0;
					resultIndex0 = 3;
					resultIndex1 = 2;
					resultIndex2 = 4;
					break;
				}

				// Calculate distances to clip the two line segments at.
				const double segment0V0Diff = vDiffs[inputIndex0];
				const double segment0V1Diff = vDiffs[inputIndex1];
				const double segment1V0Diff = vDiffs[inputIndex2];
				const double segment1V1Diff = vDiffs[inputIndex3];
				const double segment0PointT = segment0V0Diff / (segment0V0Diff - segment0V1Diff);
				const double segment1PointT = segment1V0Diff / (segment1V0Diff - segment1V1Diff);

				// Generate two vertices and texture coordinates, making sure to keep the original winding order.
				Double_LerpN<1>(currentVXs + inputIndex0, currentVXs + inputIndex1, &segment0PointT, currentVXs + GENERATED_RESULT_INDEX0);
				Double_LerpN<1>(currentVYs + inputIndex0, currentVYs + inputIndex1, &segment0PointT, currentVYs + GENERATED_RESULT_INDEX0);
				Double_LerpN<1>(currentVZs + inputIndex0, currentVZs + inputIndex1, &segment0PointT, currentVZs + GENERATED_RESULT_INDEX0);
				Double_LerpN<1>(currentVWs + inputIndex0, currentVWs + inputIndex1, &segment0PointT, currentVWs + GENERATED_RESULT_INDEX0);
				Double_LerpN<1>(currentVXs + inputIndex2, currentVXs + inputIndex3, &segment1PointT, currentVXs + GENERATED_RESULT_INDEX1);
				Double_LerpN<1>(currentVYs + inputIndex2, currentVYs + inputIndex3, &segment1PointT, currentVYs + GENERATED_RESULT_INDEX1);
				Double_LerpN<1>(currentVZs + inputIndex2, currentVZs + inputIndex3, &segment1PointT, currentVZs + GENERATED_RESULT_INDEX1);
				Double_LerpN<1>(currentVWs + inputIndex2, currentVWs + inputIndex3, &segment1PointT, currentVWs + GENERATED_RESULT_INDEX1);
				Double_LerpN<1>(currentUVXs + inputIndex0, currentUVXs + inputIndex1, &segment0PointT, currentUVXs + GENERATED_RESULT_INDEX0);
				Double_LerpN<1>(currentUVYs + inputIndex0, currentUVYs + inputIndex1, &segment0PointT, currentUVYs + GENERATED_RESULT_INDEX0);
				Double_LerpN<1>(currentUVXs + inputIndex2, currentUVXs + inputIndex3, &segment1PointT, currentUVXs + GENERATED_RESULT_INDEX1);
				Double_LerpN<1>(currentUVYs + inputIndex2, currentUVYs + inputIndex3, &segment1PointT, currentUVYs + GENERATED_RESULT_INDEX1);

				result0V0XYZW[0] = currentVXs[resultIndex0];
				result0V0XYZW[1] = currentVYs[resultIndex0];
				result0V0XYZW[2] = currentVZs[resultIndex0];
				result0V0XYZW[3] = currentVWs[resultIndex0];
				result0V1XYZW[0] = currentVXs[resultIndex1];
				result0V1XYZW[1] = currentVYs[resultIndex1];
				result0V1XYZW[2] = currentVZs[resultIndex1];
				result0V1XYZW[3] = currentVWs[resultIndex1];
				result0V2XYZW[0] = currentVXs[resultIndex2];
				result0V2XYZW[1] = currentVYs[resultIndex2];
				result0V2XYZW[2] = currentVZs[resultIndex2];
				result0V2XYZW[3] = currentVWs[resultIndex2];
				result0UV0XY[0] = currentUVXs[resultIndex0];
				result0UV0XY[1] = currentUVYs[resultIndex0];
				result0UV1XY[0] = currentUVXs[resultIndex1];
				result0UV1XY[1] = currentUVYs[resultIndex1];
				result0UV2XY[0] = currentUVXs[resultIndex2];
				result0UV2XY[1] = currentUVYs[resultIndex2];

				if (becomesQuad)
				{
					result1V0XYZW[0] = currentVXs[resultIndex3];
					result1V0XYZW[1] = currentVYs[resultIndex3];
					result1V0XYZW[2] = currentVZs[resultIndex3];
					result1V0XYZW[3] = currentVWs[resultIndex3];
					result1V1XYZW[0] = currentVXs[resultIndex4];
					result1V1XYZW[1] = currentVYs[resultIndex4];
					result1V1XYZW[2] = currentVZs[resultIndex4];
					result1V1XYZW[3] = currentVWs[resultIndex4];
					result1V2XYZW[0] = currentVXs[resultIndex5];
					result1V2XYZW[1] = currentVYs[resultIndex5];
					result1V2XYZW[2] = currentVZs[resultIndex5];
					result1V2XYZW[3] = currentVWs[resultIndex5];
					result1UV0XY[0] = currentUVXs[resultIndex3];
					result1UV0XY[1] = currentUVYs[resultIndex3];
					result1UV1XY[0] = currentUVXs[resultIndex4];
					result1UV1XY[1] = currentUVYs[resultIndex4];
					result1UV2XY[0] = currentUVXs[resultIndex5];
					result1UV2XY[1] = currentUVYs[resultIndex5];
				}
			}

			clipListSize += clipResultCount;
			clipListFrontIndex++;
		}
	}

	// Clips triangles to the frustum then writes out clip space triangle indices for the rasterizer to iterate.
	void ProcessClipping(int meshCount)
	{
		for (int meshIndex = 0; meshIndex < meshCount; meshIndex++)
		{
			const auto &shadedV0XYZWs = g_meshProcessCaches.shadedV0XYZWArrays[meshIndex];
			const auto &shadedV1XYZWs = g_meshProcessCaches.shadedV1XYZWArrays[meshIndex];
			const auto &shadedV2XYZWs = g_meshProcessCaches.shadedV2XYZWArrays[meshIndex];
			const auto &uv0XYs = g_meshProcessCaches.uv0XYArrays[meshIndex];
			const auto &uv1XYs = g_meshProcessCaches.uv1XYArrays[meshIndex];
			const auto &uv2XYs = g_meshProcessCaches.uv2XYArrays[meshIndex];
			auto &clipSpaceTriangleV0XYZWs = g_meshProcessCaches.clipSpaceTriangleV0XYZWArrays[meshIndex];
			auto &clipSpaceTriangleV1XYZWs = g_meshProcessCaches.clipSpaceTriangleV1XYZWArrays[meshIndex];
			auto &clipSpaceTriangleV2XYZWs = g_meshProcessCaches.clipSpaceTriangleV2XYZWArrays[meshIndex];
			auto &clipSpaceTriangleUV0XYs = g_meshProcessCaches.clipSpaceTriangleUV0XYArrays[meshIndex];
			auto &clipSpaceTriangleUV1XYs = g_meshProcessCaches.clipSpaceTriangleUV1XYArrays[meshIndex];
			auto &clipSpaceTriangleUV2XYs = g_meshProcessCaches.clipSpaceTriangleUV2XYArrays[meshIndex];
			auto &clipSpaceMeshV0XYZWs = g_meshProcessCaches.clipSpaceMeshV0XYZWArrays[meshIndex];
			auto &clipSpaceMeshV1XYZWs = g_meshProcessCaches.clipSpaceMeshV1XYZWArrays[meshIndex];
			auto &clipSpaceMeshV2XYZWs = g_meshProcessCaches.clipSpaceMeshV2XYZWArrays[meshIndex];
			auto &clipSpaceMeshUV0XYs = g_meshProcessCaches.clipSpaceMeshUV0XYArrays[meshIndex];
			auto &clipSpaceMeshUV1XYs = g_meshProcessCaches.clipSpaceMeshUV1XYArrays[meshIndex];
			auto &clipSpaceMeshUV2XYs = g_meshProcessCaches.clipSpaceMeshUV2XYArrays[meshIndex];
			int &clipSpaceMeshTriangleCount = g_meshProcessCaches.clipSpaceMeshTriangleCounts[meshIndex];

			// Reset clip space cache. Skip zeroing the mesh arrays for performance.
			clipSpaceMeshTriangleCount = 0;

			// Clip each vertex-shaded triangle and save them in a cache for rasterization.
			const int triangleCount = g_meshProcessCaches.indexBuffers[meshIndex]->triangleCount;
			for (int triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++)
			{
				const auto &shadedV0XYZW = shadedV0XYZWs[triangleIndex];
				const auto &shadedV1XYZW = shadedV1XYZWs[triangleIndex];
				const auto &shadedV2XYZW = shadedV2XYZWs[triangleIndex];
				const auto &uv0XY = uv0XYs[triangleIndex];
				const auto &uv1XY = uv1XYs[triangleIndex];
				const auto &uv2XY = uv2XYs[triangleIndex];
				auto &firstClipSpaceTriangleV0XYZW = clipSpaceTriangleV0XYZWs[0];
				auto &firstClipSpaceTriangleV1XYZW = clipSpaceTriangleV1XYZWs[0];
				auto &firstClipSpaceTriangleV2XYZW = clipSpaceTriangleV2XYZWs[0];
				auto &firstClipSpaceTriangleUV0XY = clipSpaceTriangleUV0XYs[0];
				auto &firstClipSpaceTriangleUV1XY = clipSpaceTriangleUV1XYs[0];
				auto &firstClipSpaceTriangleUV2XY = clipSpaceTriangleUV2XYs[0];

				// Initialize clipping loop with the vertex-shaded triangle.
				firstClipSpaceTriangleV0XYZW[0] = shadedV0XYZW[0];
				firstClipSpaceTriangleV0XYZW[1] = shadedV0XYZW[1];
				firstClipSpaceTriangleV0XYZW[2] = shadedV0XYZW[2];
				firstClipSpaceTriangleV0XYZW[3] = shadedV0XYZW[3];
				firstClipSpaceTriangleV1XYZW[0] = shadedV1XYZW[0];
				firstClipSpaceTriangleV1XYZW[1] = shadedV1XYZW[1];
				firstClipSpaceTriangleV1XYZW[2] = shadedV1XYZW[2];
				firstClipSpaceTriangleV1XYZW[3] = shadedV1XYZW[3];
				firstClipSpaceTriangleV2XYZW[0] = shadedV2XYZW[0];
				firstClipSpaceTriangleV2XYZW[1] = shadedV2XYZW[1];
				firstClipSpaceTriangleV2XYZW[2] = shadedV2XYZW[2];
				firstClipSpaceTriangleV2XYZW[3] = shadedV2XYZW[3];
				firstClipSpaceTriangleUV0XY[0] = uv0XY[0];
				firstClipSpaceTriangleUV0XY[1] = uv0XY[1];
				firstClipSpaceTriangleUV1XY[0] = uv1XY[0];
				firstClipSpaceTriangleUV1XY[1] = uv1XY[1];
				firstClipSpaceTriangleUV2XY[0] = uv2XY[0];
				firstClipSpaceTriangleUV2XY[1] = uv2XY[1];

				int clipListSize = 1; // Triangles to process based on this vertex-shaded triangle.
				int clipListFrontIndex = 0;

				// Check each dimension against -W and W components.
				ProcessClippingWithPlane<0>(meshIndex, clipListSize, clipListFrontIndex);
				ProcessClippingWithPlane<1>(meshIndex, clipListSize, clipListFrontIndex);
				ProcessClippingWithPlane<2>(meshIndex, clipListSize, clipListFrontIndex);
				ProcessClippingWithPlane<3>(meshIndex, clipListSize, clipListFrontIndex);
				ProcessClippingWithPlane<4>(meshIndex, clipListSize, clipListFrontIndex);
				ProcessClippingWithPlane<5>(meshIndex, clipListSize, clipListFrontIndex);

				// Add the clip results to the mesh, skipping the incomplete triangles the front index advanced beyond.
				const int resultTriangleCount = clipListSize - clipListFrontIndex;
				for (int resultTriangleIndex = 0; resultTriangleIndex < resultTriangleCount; resultTriangleIndex++)
				{
					const int srcIndex = clipListFrontIndex + resultTriangleIndex;
					const auto &clipSpaceTriangleV0XYZW = clipSpaceTriangleV0XYZWs[srcIndex];
					const auto &clipSpaceTriangleV1XYZW = clipSpaceTriangleV1XYZWs[srcIndex];
					const auto &clipSpaceTriangleV2XYZW = clipSpaceTriangleV2XYZWs[srcIndex];
					const auto &clipSpaceTriangleUV0XY = clipSpaceTriangleUV0XYs[srcIndex];
					const auto &clipSpaceTriangleUV1XY = clipSpaceTriangleUV1XYs[srcIndex];
					const auto &clipSpaceTriangleUV2XY = clipSpaceTriangleUV2XYs[srcIndex];

					const int dstIndex = clipSpaceMeshTriangleCount + resultTriangleIndex;
					auto &clipSpaceMeshV0XYZW = clipSpaceMeshV0XYZWs[dstIndex];
					auto &clipSpaceMeshV1XYZW = clipSpaceMeshV1XYZWs[dstIndex];
					auto &clipSpaceMeshV2XYZW = clipSpaceMeshV2XYZWs[dstIndex];
					auto &clipSpaceMeshUV0XY = clipSpaceMeshUV0XYs[dstIndex];
					auto &clipSpaceMeshUV1XY = clipSpaceMeshUV1XYs[dstIndex];
					auto &clipSpaceMeshUV2XY = clipSpaceMeshUV2XYs[dstIndex];

					clipSpaceMeshV0XYZW[0] = clipSpaceTriangleV0XYZW[0];
					clipSpaceMeshV0XYZW[1] = clipSpaceTriangleV0XYZW[1];
					clipSpaceMeshV0XYZW[2] = clipSpaceTriangleV0XYZW[2];
					clipSpaceMeshV0XYZW[3] = clipSpaceTriangleV0XYZW[3];
					clipSpaceMeshV1XYZW[0] = clipSpaceTriangleV1XYZW[0];
					clipSpaceMeshV1XYZW[1] = clipSpaceTriangleV1XYZW[1];
					clipSpaceMeshV1XYZW[2] = clipSpaceTriangleV1XYZW[2];
					clipSpaceMeshV1XYZW[3] = clipSpaceTriangleV1XYZW[3];
					clipSpaceMeshV2XYZW[0] = clipSpaceTriangleV2XYZW[0];
					clipSpaceMeshV2XYZW[1] = clipSpaceTriangleV2XYZW[1];
					clipSpaceMeshV2XYZW[2] = clipSpaceTriangleV2XYZW[2];
					clipSpaceMeshV2XYZW[3] = clipSpaceTriangleV2XYZW[3];
					clipSpaceMeshUV0XY[0] = clipSpaceTriangleUV0XY[0];
					clipSpaceMeshUV0XY[1] = clipSpaceTriangleUV0XY[1];
					clipSpaceMeshUV1XY[0] = clipSpaceTriangleUV1XY[0];
					clipSpaceMeshUV1XY[1] = clipSpaceTriangleUV1XY[1];
					clipSpaceMeshUV2XY[0] = clipSpaceTriangleUV2XY[0];
					clipSpaceMeshUV2XY[1] = clipSpaceTriangleUV2XY[1];
				}

				clipSpaceMeshTriangleCount += resultTriangleCount;
			}
		}
	}
}

// Rasterizer, pixel shader execution.
namespace
{
	struct RasterizerTriangle
	{
		// The rasterizer prefers vertices in AoS layout.
		double clip0X, clip0Y, clip0Z, clip0W;
		double clip1X, clip1Y, clip1Z, clip1W;
		double clip2X, clip2Y, clip2Z, clip2W;
		double clip0WRecip;
		double clip1WRecip;
		double clip2WRecip;
		double ndc0X, ndc0Y, ndc0Z;
		double ndc1X, ndc1Y, ndc1Z;
		double ndc2X, ndc2Y, ndc2Z;
		double screenSpace0X, screenSpace0Y;
		double screenSpace1X, screenSpace1Y;
		double screenSpace2X, screenSpace2Y;
		double screenSpace01X, screenSpace01Y;
		double screenSpace12X, screenSpace12Y;
		double screenSpace20X, screenSpace20Y;
		double screenSpace01PerpX, screenSpace01PerpY;
		double screenSpace12PerpX, screenSpace12PerpY;
		double screenSpace20PerpX, screenSpace20PerpY;
		double uv0X, uv0Y;
		double uv1X, uv1Y;
		double uv2X, uv2Y;
		double uv0XDivW, uv0YDivW;
		double uv1XDivW, uv1YDivW;
		double uv2XDivW, uv2YDivW;

		// Naive bounding box.
		int xStart, xEnd;
		int yStart, yEnd;
	};

	struct RasterizerCache
	{
		RasterizerTriangle visibleTriangleLists[MAX_MESH_PROCESS_CACHES][MAX_CLIPPED_MESH_TRIANGLES];
		int triangleCounts[MAX_MESH_PROCESS_CACHES];
	};

	RasterizerCache g_rasterizerCache;

	void ProcessClipSpaceTrianglesForFrontFacing(int meshCount)
	{
		for (int meshIndex = 0; meshIndex < meshCount; meshIndex++)
		{
			const auto &clipSpaceMeshV0XYZWs = g_meshProcessCaches.clipSpaceMeshV0XYZWArrays[meshIndex];
			const auto &clipSpaceMeshV1XYZWs = g_meshProcessCaches.clipSpaceMeshV1XYZWArrays[meshIndex];
			const auto &clipSpaceMeshV2XYZWs = g_meshProcessCaches.clipSpaceMeshV2XYZWArrays[meshIndex];
			const auto &clipSpaceMeshUV0XYs = g_meshProcessCaches.clipSpaceMeshUV0XYArrays[meshIndex];
			const auto &clipSpaceMeshUV1XYs = g_meshProcessCaches.clipSpaceMeshUV1XYArrays[meshIndex];
			const auto &clipSpaceMeshUV2XYs = g_meshProcessCaches.clipSpaceMeshUV2XYArrays[meshIndex];
			auto &rasterizerTriangles = g_rasterizerCache.visibleTriangleLists[meshIndex];

			int writeIndex = 0;
			const int triangleCount = g_meshProcessCaches.clipSpaceMeshTriangleCounts[meshIndex];
			for (int triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++)
			{
				const auto &clipSpaceMeshV0XYZW = clipSpaceMeshV0XYZWs[triangleIndex];
				const auto &clipSpaceMeshV1XYZW = clipSpaceMeshV1XYZWs[triangleIndex];
				const auto &clipSpaceMeshV2XYZW = clipSpaceMeshV2XYZWs[triangleIndex];
				const double clip0X = clipSpaceMeshV0XYZW[0];
				const double clip0Y = clipSpaceMeshV0XYZW[1];
				const double clip0Z = clipSpaceMeshV0XYZW[2];
				const double clip0W = clipSpaceMeshV0XYZW[3];
				const double clip1X = clipSpaceMeshV1XYZW[0];
				const double clip1Y = clipSpaceMeshV1XYZW[1];
				const double clip1Z = clipSpaceMeshV1XYZW[2];
				const double clip1W = clipSpaceMeshV1XYZW[3];
				const double clip2X = clipSpaceMeshV2XYZW[0];
				const double clip2Y = clipSpaceMeshV2XYZW[1];
				const double clip2Z = clipSpaceMeshV2XYZW[2];
				const double clip2W = clipSpaceMeshV2XYZW[3];
				const double clip0WRecip = 1.0 / clip0W;
				const double clip1WRecip = 1.0 / clip1W;
				const double clip2WRecip = 1.0 / clip2W;
				const double ndc0X = clip0X * clip0WRecip;
				const double ndc0Y = clip0Y * clip0WRecip;
				const double ndc0Z = clip0Z * clip0WRecip;
				const double ndc1X = clip1X * clip1WRecip;
				const double ndc1Y = clip1Y * clip1WRecip;
				const double ndc1Z = clip1Z * clip1WRecip;
				const double ndc2X = clip2X * clip2WRecip;
				const double ndc2Y = clip2Y * clip2WRecip;
				const double ndc2Z = clip2Z * clip2WRecip;
				const double screenSpace0X = NdcXToScreenSpace(ndc0X, g_frameBufferWidthReal);
				const double screenSpace0Y = NdcYToScreenSpace(ndc0Y, g_frameBufferHeightReal);
				const double screenSpace1X = NdcXToScreenSpace(ndc1X, g_frameBufferWidthReal);
				const double screenSpace1Y = NdcYToScreenSpace(ndc1Y, g_frameBufferHeightReal);
				const double screenSpace2X = NdcXToScreenSpace(ndc2X, g_frameBufferWidthReal);
				const double screenSpace2Y = NdcYToScreenSpace(ndc2Y, g_frameBufferHeightReal);
				const double screenSpace01X = screenSpace1X - screenSpace0X;
				const double screenSpace01Y = screenSpace1Y - screenSpace0Y;
				const double screenSpace12X = screenSpace2X - screenSpace1X;
				const double screenSpace12Y = screenSpace2Y - screenSpace1Y;
				const double screenSpace20X = screenSpace0X - screenSpace2X;
				const double screenSpace20Y = screenSpace0Y - screenSpace2Y;

				double screenSpace01Cross12, screenSpace12Cross20, screenSpace20Cross01;
				Double2_CrossN<1>(&screenSpace12X, &screenSpace12Y, &screenSpace01X, &screenSpace01Y, &screenSpace01Cross12);
				Double2_CrossN<1>(&screenSpace20X, &screenSpace20Y, &screenSpace12X, &screenSpace12Y, &screenSpace12Cross20);
				Double2_CrossN<1>(&screenSpace01X, &screenSpace01Y, &screenSpace20X, &screenSpace20Y, &screenSpace20Cross01);

				// Discard back-facing.
				const bool isFrontFacing = (screenSpace01Cross12 + screenSpace12Cross20 + screenSpace20Cross01) > 0.0;
				if (!isFrontFacing)
				{
					continue;
				}

				// Naive screen-space bounding box around triangle.
				const double xMin = std::min(screenSpace0X, std::min(screenSpace1X, screenSpace2X));
				const double xMax = std::max(screenSpace0X, std::max(screenSpace1X, screenSpace2X));
				const double yMin = std::min(screenSpace0Y, std::min(screenSpace1Y, screenSpace2Y));
				const double yMax = std::max(screenSpace0Y, std::max(screenSpace1Y, screenSpace2Y));
				const int xStart = RendererUtils::getLowerBoundedPixel(xMin, g_frameBufferWidth);
				const int xEnd = RendererUtils::getUpperBoundedPixel(xMax, g_frameBufferWidth);
				const int yStart = RendererUtils::getLowerBoundedPixel(yMin, g_frameBufferHeight);
				const int yEnd = RendererUtils::getUpperBoundedPixel(yMax, g_frameBufferHeight);

				const bool hasPositiveScreenArea = (xEnd > xStart) && (yEnd > yStart);
				if (!hasPositiveScreenArea)
				{
					continue;
				}

				g_totalPresentedTriangleCount++;

				double screenSpace01PerpX, screenSpace01PerpY;
				double screenSpace12PerpX, screenSpace12PerpY;
				double screenSpace20PerpX, screenSpace20PerpY;
				Double2_RightPerpN<1>(&screenSpace01X, &screenSpace01Y, &screenSpace01PerpX, &screenSpace01PerpY);
				Double2_RightPerpN<1>(&screenSpace12X, &screenSpace12Y, &screenSpace12PerpX, &screenSpace12PerpY);
				Double2_RightPerpN<1>(&screenSpace20X, &screenSpace20Y, &screenSpace20PerpX, &screenSpace20PerpY);

				const auto &clipSpaceMeshUV0XY = clipSpaceMeshUV0XYs[triangleIndex];
				const auto &clipSpaceMeshUV1XY = clipSpaceMeshUV1XYs[triangleIndex];
				const auto &clipSpaceMeshUV2XY = clipSpaceMeshUV2XYs[triangleIndex];
				const double uv0X = clipSpaceMeshUV0XY[0];
				const double uv0Y = clipSpaceMeshUV0XY[1];
				const double uv1X = clipSpaceMeshUV1XY[0];
				const double uv1Y = clipSpaceMeshUV1XY[1];
				const double uv2X = clipSpaceMeshUV2XY[0];
				const double uv2Y = clipSpaceMeshUV2XY[1];
				const double uv0XDivW = uv0X * clip0WRecip;
				const double uv0YDivW = uv0Y * clip0WRecip;
				const double uv1XDivW = uv1X * clip1WRecip;
				const double uv1YDivW = uv1Y * clip1WRecip;
				const double uv2XDivW = uv2X * clip2WRecip;
				const double uv2YDivW = uv2Y * clip2WRecip;

				RasterizerTriangle &triangle = rasterizerTriangles[writeIndex];
				triangle.clip0X = clip0X;
				triangle.clip0Y = clip0Y;
				triangle.clip0Z = clip0Z;
				triangle.clip0W = clip0W;
				triangle.clip1X = clip1X;
				triangle.clip1Y = clip1Y;
				triangle.clip1Z = clip1Z;
				triangle.clip1W = clip1W;
				triangle.clip2X = clip2X;
				triangle.clip2Y = clip2Y;
				triangle.clip2Z = clip2Z;
				triangle.clip2W = clip2W;
				triangle.clip0WRecip = clip0WRecip;
				triangle.clip1WRecip = clip1WRecip;
				triangle.clip2WRecip = clip2WRecip;
				triangle.ndc0X = ndc0X;
				triangle.ndc0Y = ndc0Y;
				triangle.ndc0Z = ndc0Z;
				triangle.ndc1X = ndc1X;
				triangle.ndc1Y = ndc1Y;
				triangle.ndc1Z = ndc1Z;
				triangle.ndc2X = ndc2X;
				triangle.ndc2Y = ndc2Y;
				triangle.ndc2Z = ndc2Z;
				triangle.screenSpace0X = screenSpace0X;
				triangle.screenSpace0Y = screenSpace0Y;
				triangle.screenSpace1X = screenSpace1X;
				triangle.screenSpace1Y = screenSpace1Y;
				triangle.screenSpace2X = screenSpace2X;
				triangle.screenSpace2Y = screenSpace2Y;
				triangle.screenSpace01X = screenSpace01X;
				triangle.screenSpace01Y = screenSpace01Y;
				triangle.screenSpace12X = screenSpace12X;
				triangle.screenSpace12Y = screenSpace12Y;
				triangle.screenSpace20X = screenSpace20X;
				triangle.screenSpace20Y = screenSpace20Y;
				triangle.screenSpace01PerpX = screenSpace01PerpX;
				triangle.screenSpace01PerpY = screenSpace01PerpY;
				triangle.screenSpace12PerpX = screenSpace12PerpX;
				triangle.screenSpace12PerpY = screenSpace12PerpY;
				triangle.screenSpace20PerpX = screenSpace20PerpX;
				triangle.screenSpace20PerpY = screenSpace20PerpY;
				triangle.uv0X = uv0X;
				triangle.uv0Y = uv0Y;
				triangle.uv1X = uv1X;
				triangle.uv1Y = uv1Y;
				triangle.uv2X = uv2X;
				triangle.uv2Y = uv2Y;
				triangle.uv0XDivW = uv0XDivW;
				triangle.uv0YDivW = uv0YDivW;
				triangle.uv1XDivW = uv1XDivW;
				triangle.uv1YDivW = uv1YDivW;
				triangle.uv2XDivW = uv2XDivW;
				triangle.uv2YDivW = uv2YDivW;
				triangle.xStart = xStart;
				triangle.xEnd = xEnd;
				triangle.yStart = yStart;
				triangle.yEnd = yEnd;

				writeIndex++;
			}

			g_rasterizerCache.triangleCounts[meshIndex] = writeIndex;
		}
	}

	template<RenderLightingType lightingType, PixelShaderType pixelShaderType, bool enableDepthRead, bool enableDepthWrite, DitheringMode ditheringMode>
	void RasterizeMeshInternal(int meshIndex, const SoftwareRenderer::ObjectTexturePool &textures)
	{
		const TextureSamplingType textureSamplingType0 = g_meshProcessCaches.textureSamplingType0s[meshIndex];
		const TextureSamplingType textureSamplingType1 = g_meshProcessCaches.textureSamplingType1s[meshIndex];
		const double meshLightPercent = g_meshProcessCaches.meshLightPercents[meshIndex];
		const auto &lights = g_meshProcessCaches.lightPtrArrays[meshIndex];
		const int lightCount = g_meshProcessCaches.lightCounts[meshIndex];
		const double pixelShaderParam0 = g_meshProcessCaches.pixelShaderParam0s[meshIndex];

		constexpr int RASTERIZE_LOOP_UNROLL_X = WEAK_LOOP_UNROLL;
		constexpr int RASTERIZE_LOOP_UNROLL_Y = WEAK_LOOP_UNROLL;

		constexpr bool requiresTwoTextures = (pixelShaderType == PixelShaderType::OpaqueWithAlphaTestLayer) || (pixelShaderType == PixelShaderType::AlphaTestedWithPaletteIndexLookup);
		constexpr bool requiresHorizonMirror = pixelShaderType == PixelShaderType::AlphaTestedWithHorizonMirror;
		constexpr bool requiresPerPixelLightIntensity = lightingType == RenderLightingType::PerPixel;
		constexpr bool requiresPerMeshLightIntensity = lightingType == RenderLightingType::PerMesh;

		PixelShaderLighting shaderLighting;
		shaderLighting.lightTableTexels = g_lightTableTexture->texels8Bit;
		shaderLighting.lightLevelCount = g_lightTableTexture->height;
		shaderLighting.lightLevelCountReal = static_cast<double>(shaderLighting.lightLevelCount);
		shaderLighting.lastLightLevel = shaderLighting.lightLevelCount - 1;
		shaderLighting.texelsPerLightLevel = g_lightTableTexture->width;
		shaderLighting.lightLevel = 0;

		PixelShaderFrameBuffer shaderFrameBuffer;
		shaderFrameBuffer.palette.colors = g_paletteTexture->texels32Bit;
		shaderFrameBuffer.palette.count = g_paletteTexture->texelCount;

		PixelShaderHorizonMirror shaderHorizonMirror;
		if constexpr (requiresHorizonMirror)
		{
			const Double2 horizonScreenSpacePoint = RendererUtils::ndcToScreenSpace(g_horizonNdcPoint, g_frameBufferWidthReal, g_frameBufferHeightReal);
			shaderHorizonMirror.horizonScreenSpacePointX = horizonScreenSpacePoint.x;
			shaderHorizonMirror.horizonScreenSpacePointY = horizonScreenSpacePoint.y;

			DebugAssert(g_skyBgTexture->texelCount > 0);
			shaderHorizonMirror.fallbackSkyColor = g_skyBgTexture->texels8Bit[0];
		}

		const ObjectTextureID textureID0 = g_meshProcessCaches.textureID0s[meshIndex];
		const ObjectTextureID textureID1 = g_meshProcessCaches.textureID1s[meshIndex];

		const SoftwareRenderer::ObjectTexture &texture0 = textures.get(textureID0);
		PixelShaderTexture shaderTexture0;
		shaderTexture0.init(texture0.texels8Bit, texture0.width, texture0.height, textureSamplingType0);

		PixelShaderTexture shaderTexture1;
		if constexpr (requiresTwoTextures)
		{
			const SoftwareRenderer::ObjectTexture &texture1 = textures.get(textureID1);
			shaderTexture1.init(texture1.texels8Bit, texture1.width, texture1.height, textureSamplingType1);
		}

		const auto &triangles = g_rasterizerCache.visibleTriangleLists[meshIndex];
		const int triangleCount = g_rasterizerCache.triangleCounts[meshIndex];
		for (int triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++)
		{
			const RasterizerTriangle &triangle = triangles[triangleIndex];
			const double clip0X = triangle.clip0X;
			const double clip0Y = triangle.clip0Y;
			const double clip0Z = triangle.clip0Z;
			const double clip0W = triangle.clip0W;
			const double clip1X = triangle.clip1X;
			const double clip1Y = triangle.clip1Y;
			const double clip1Z = triangle.clip1Z;
			const double clip1W = triangle.clip1W;
			const double clip2X = triangle.clip2X;
			const double clip2Y = triangle.clip2Y;
			const double clip2Z = triangle.clip2Z;
			const double clip2W = triangle.clip2W;
			const double clip0WRecip = triangle.clip0WRecip;
			const double clip1WRecip = triangle.clip1WRecip;
			const double clip2WRecip = triangle.clip2WRecip;
			const double ndc0X = triangle.ndc0X;
			const double ndc0Y = triangle.ndc0Y;
			const double ndc0Z = triangle.ndc0Z;
			const double ndc1X = triangle.ndc1X;
			const double ndc1Y = triangle.ndc1Y;
			const double ndc1Z = triangle.ndc1Z;
			const double ndc2X = triangle.ndc2X;
			const double ndc2Y = triangle.ndc2Y;
			const double ndc2Z = triangle.ndc2Z;
			const double screenSpace0X = triangle.screenSpace0X;
			const double screenSpace0Y = triangle.screenSpace0Y;
			const double screenSpace1X = triangle.screenSpace1X;
			const double screenSpace1Y = triangle.screenSpace1Y;
			const double screenSpace2X = triangle.screenSpace2X;
			const double screenSpace2Y = triangle.screenSpace2Y;
			const double screenSpace01X = triangle.screenSpace01X;
			const double screenSpace01Y = triangle.screenSpace01Y;
			const double screenSpace12X = triangle.screenSpace12X;
			const double screenSpace12Y = triangle.screenSpace12Y;
			const double screenSpace20X = triangle.screenSpace20X;
			const double screenSpace20Y = triangle.screenSpace20Y;
			const double screenSpace01PerpX = triangle.screenSpace01PerpX;
			const double screenSpace01PerpY = triangle.screenSpace01PerpY;
			const double screenSpace12PerpX = triangle.screenSpace12PerpX;
			const double screenSpace12PerpY = triangle.screenSpace12PerpY;
			const double screenSpace20PerpX = triangle.screenSpace20PerpX;
			const double screenSpace20PerpY = triangle.screenSpace20PerpY;
			const double uv0X = triangle.uv0X;
			const double uv0Y = triangle.uv0Y;
			const double uv1X = triangle.uv1X;
			const double uv1Y = triangle.uv1Y;
			const double uv2X = triangle.uv2X;
			const double uv2Y = triangle.uv2Y;
			const double uv0XDivW = triangle.uv0XDivW;
			const double uv0YDivW = triangle.uv0YDivW;
			const double uv1XDivW = triangle.uv1XDivW;
			const double uv1YDivW = triangle.uv1YDivW;
			const double uv2XDivW = triangle.uv2XDivW;
			const double uv2YDivW = triangle.uv2YDivW;
			const int xStart = triangle.xStart;
			const int xEnd = triangle.xEnd;
			const int yStart = triangle.yStart;
			const int yEnd = triangle.yEnd;

			for (int y = yStart; y < yEnd; y++)
			{
				shaderFrameBuffer.yPercent = (static_cast<double>(y) + 0.50) * g_frameBufferHeightRealRecip;

				for (int x = xStart; x < xEnd; x++)
				{
					shaderFrameBuffer.xPercent = (static_cast<double>(x) + 0.50) * g_frameBufferWidthRealRecip;
					shaderFrameBuffer.pixelIndex = x + (y * g_frameBufferWidth);
					const double pixelCenterX = shaderFrameBuffer.xPercent * g_frameBufferWidthReal;
					const double pixelCenterY = shaderFrameBuffer.yPercent * g_frameBufferHeightReal;

					// See if pixel center is inside triangle.
					const bool inHalfSpace0 = IsScreenSpacePointInHalfSpace(pixelCenterX, pixelCenterY, screenSpace0X, screenSpace0Y, screenSpace01PerpX, screenSpace01PerpY);
					const bool inHalfSpace1 = IsScreenSpacePointInHalfSpace(pixelCenterX, pixelCenterY, screenSpace1X, screenSpace1Y, screenSpace12PerpX, screenSpace12PerpY);
					const bool inHalfSpace2 = IsScreenSpacePointInHalfSpace(pixelCenterX, pixelCenterY, screenSpace2X, screenSpace2Y, screenSpace20PerpX, screenSpace20PerpY);
					if (inHalfSpace0 && inHalfSpace1 && inHalfSpace2)
					{
						const double ss0X = screenSpace01X;
						const double ss0Y = screenSpace01Y;
						const double ss1X = screenSpace2X - screenSpace0X;
						const double ss1Y = screenSpace2Y - screenSpace0Y;
						const double ss2X = pixelCenterX - screenSpace0X;
						const double ss2Y = pixelCenterY - screenSpace0Y;

						double dot00, dot01, dot11, dot20, dot21;
						Double2_DotN<1>(&ss0X, &ss0Y, &ss0X, &ss0Y, &dot00);
						Double2_DotN<1>(&ss0X, &ss0Y, &ss1X, &ss1Y, &dot01);
						Double2_DotN<1>(&ss1X, &ss1Y, &ss1X, &ss1Y, &dot11);
						Double2_DotN<1>(&ss2X, &ss2Y, &ss0X, &ss0Y, &dot20);
						Double2_DotN<1>(&ss2X, &ss2Y, &ss1X, &ss1Y, &dot21);

						const double denominator = (dot00 * dot11) - (dot01 * dot01);
						const double denominatorRecip = 1.0 / denominator;
						const double vNumerator = (dot11 * dot20) - (dot01 * dot21);
						const double wNumerator = (dot00 * dot21) - (dot01 * dot20);
						const double v = vNumerator * denominatorRecip;
						const double w = wNumerator * denominatorRecip;
						const double u = 1.0 - v - w;

						PixelShaderPerspectiveCorrection shaderPerspective;
						shaderPerspective.ndcZDepth = (ndc0Z * u) + (ndc1Z * v) + (ndc2Z * w);

						bool passesDepthTest = true;
						if constexpr (enableDepthRead)
						{
							passesDepthTest = shaderPerspective.ndcZDepth < g_depthBuffer[shaderFrameBuffer.pixelIndex];
							g_totalDepthTests++;
						}

						if (passesDepthTest)
						{
							const double shaderClipSpacePointX = (ndc0X * u) + (ndc1X * v) + (ndc2X * w);
							const double shaderClipSpacePointY = (ndc0Y * u) + (ndc1Y * v) + (ndc2Y * w);
							const double shaderClipSpacePointZ = (ndc0Z * u) + (ndc1Z * v) + (ndc2Z * w);
							const double shaderClipSpacePointW = (clip0WRecip * u) + (clip1WRecip * v) + (clip2WRecip * w);
							const double shaderClipSpacePointWRecip = 1.0 / shaderClipSpacePointW;
							const double shaderHomogeneousSpacePointX = shaderClipSpacePointX * shaderClipSpacePointWRecip;
							const double shaderHomogeneousSpacePointY = shaderClipSpacePointY * shaderClipSpacePointWRecip;
							const double shaderHomogeneousSpacePointZ = shaderClipSpacePointZ * shaderClipSpacePointWRecip;
							const double shaderHomogeneousSpacePointW = shaderClipSpacePointWRecip;

							// Apply homogeneous-to-camera space transform.
							double shaderCameraSpacePointX = 0.0;
							double shaderCameraSpacePointY = 0.0;
							double shaderCameraSpacePointZ = 0.0;
							double shaderCameraSpacePointW = 0.0;
							Matrix4_MultiplyVectorN<1>(
								g_invProjMatrixXX, g_invProjMatrixXY, g_invProjMatrixXZ, g_invProjMatrixXW,
								g_invProjMatrixYX, g_invProjMatrixYY, g_invProjMatrixYZ, g_invProjMatrixYW,
								g_invProjMatrixZX, g_invProjMatrixZY, g_invProjMatrixZZ, g_invProjMatrixZW,
								g_invProjMatrixWX, g_invProjMatrixWY, g_invProjMatrixWZ, g_invProjMatrixWW,
								&shaderHomogeneousSpacePointX, &shaderHomogeneousSpacePointY, &shaderHomogeneousSpacePointZ, &shaderHomogeneousSpacePointW,
								&shaderCameraSpacePointX, &shaderCameraSpacePointY, &shaderCameraSpacePointZ, &shaderCameraSpacePointW);

							// Apply camera-to-world space transform.
							double shaderWorldSpacePointX = 0.0;
							double shaderWorldSpacePointY = 0.0;
							double shaderWorldSpacePointZ = 0.0;
							double shaderWorldSpacePointW = 0.0;
							Matrix4_MultiplyVectorN<1>(
								g_invViewMatrixXX, g_invViewMatrixXY, g_invViewMatrixXZ, g_invViewMatrixXW,
								g_invViewMatrixYX, g_invViewMatrixYY, g_invViewMatrixYZ, g_invViewMatrixYW,
								g_invViewMatrixZX, g_invViewMatrixZY, g_invViewMatrixZZ, g_invViewMatrixZW,
								g_invViewMatrixWX, g_invViewMatrixWY, g_invViewMatrixWZ, g_invViewMatrixWW,
								&shaderCameraSpacePointX, &shaderCameraSpacePointY, &shaderCameraSpacePointZ, &shaderCameraSpacePointW,
								&shaderWorldSpacePointX, &shaderWorldSpacePointY, &shaderWorldSpacePointZ, &shaderWorldSpacePointW);

							shaderPerspective.texelPercentX = ((uv0XDivW * u) + (uv1XDivW * v) + (uv2XDivW * w)) * shaderClipSpacePointWRecip;
							shaderPerspective.texelPercentY = ((uv0YDivW * u) + (uv1YDivW * v) + (uv2YDivW * w)) * shaderClipSpacePointWRecip;

							double lightIntensitySum = 0.0;
							if constexpr (requiresPerPixelLightIntensity)
							{
								lightIntensitySum = g_ambientPercent;
								for (int lightIndex = 0; lightIndex < lightCount; lightIndex++)
								{
									const SoftwareRenderer::Light &light = *lights[lightIndex];
									const double lightPointDiffX = light.worldPointX - shaderWorldSpacePointX;
									const double lightPointDiffY = light.worldPointY - shaderWorldSpacePointY;
									const double lightPointDiffZ = light.worldPointZ - shaderWorldSpacePointZ;
									const double lightDistanceSqr = (lightPointDiffX * lightPointDiffX) + (lightPointDiffY * lightPointDiffY) + (lightPointDiffZ * lightPointDiffZ);
									const double lightDistance = std::sqrt(lightDistanceSqr);
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

							if constexpr (requiresPerPixelLightIntensity)
							{
								// Dither the light level in screen space.
								bool shouldDither;
								if constexpr (ditheringMode == DitheringMode::None)
								{
									shouldDither = false;
								}
								else if (ditheringMode == DitheringMode::Classic)
								{
									shouldDither = g_ditherBuffer[shaderFrameBuffer.pixelIndex];
								}
								else if (ditheringMode == DitheringMode::Modern)
								{
									if (lightIntensitySum < 1.0) // Keeps from dithering right next to the camera, not sure why the lowest dither level doesn't do this.
									{
										constexpr int maskCount = DITHERING_MODERN_MASK_COUNT;
										const double lightLevelFraction = lightLevelReal - std::floor(lightLevelReal);
										const int maskIndex = std::clamp(static_cast<int>(static_cast<double>(maskCount) * lightLevelFraction), 0, maskCount - 1);
										const int ditherBufferIndex = shaderFrameBuffer.pixelIndex + (maskIndex * g_frameBufferPixelCount);
										shouldDither = g_ditherBuffer[ditherBufferIndex];
									}
									else
									{
										shouldDither = false;
									}
								}
								else
								{
									shouldDither = false;
								}

								if (shouldDither)
								{
									shaderLighting.lightLevel = std::min(shaderLighting.lightLevel + 1, shaderLighting.lastLightLevel);
								}
							}

							if constexpr (requiresHorizonMirror)
							{
								// @todo: support camera roll
								const double reflectedScreenSpacePointX = pixelCenterX;
								const double reflectedScreenSpacePointY = shaderHorizonMirror.horizonScreenSpacePointY + (shaderHorizonMirror.horizonScreenSpacePointY - pixelCenterY);

								const int reflectedPixelX = static_cast<int>(reflectedScreenSpacePointX);
								const int reflectedPixelY = static_cast<int>(reflectedScreenSpacePointY);
								shaderHorizonMirror.isReflectedPixelInFrameBuffer =
									(reflectedPixelX >= 0) && (reflectedPixelX < g_frameBufferWidth) &&
									(reflectedPixelY >= 0) && (reflectedPixelY < g_frameBufferHeight);
								shaderHorizonMirror.reflectedPixelIndex = reflectedPixelX + (reflectedPixelY * g_frameBufferWidth);
							}

							if constexpr (pixelShaderType == PixelShaderType::Opaque)
							{
								PixelShader_Opaque<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::OpaqueWithAlphaTestLayer)
							{
								PixelShader_OpaqueWithAlphaTestLayer<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderTexture1, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTested)
							{
								PixelShader_AlphaTested<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithVariableTexCoordUMin)
							{
								PixelShader_AlphaTestedWithVariableTexCoordUMin<enableDepthWrite>(shaderPerspective, shaderTexture0, pixelShaderParam0, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithVariableTexCoordVMin)
							{
								PixelShader_AlphaTestedWithVariableTexCoordVMin<enableDepthWrite>(shaderPerspective, shaderTexture0, pixelShaderParam0, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithPaletteIndexLookup)
							{
								PixelShader_AlphaTestedWithPaletteIndexLookup<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderTexture1, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithLightLevelColor)
							{
								PixelShader_AlphaTestedWithLightLevelColor<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithLightLevelOpacity)
							{
								PixelShader_AlphaTestedWithLightLevelOpacity<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderLighting, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithPreviousBrightnessLimit)
							{
								PixelShader_AlphaTestedWithPreviousBrightnessLimit<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderFrameBuffer);
							}
							else if (pixelShaderType == PixelShaderType::AlphaTestedWithHorizonMirror)
							{
								PixelShader_AlphaTestedWithHorizonMirror<enableDepthWrite>(shaderPerspective, shaderTexture0, shaderHorizonMirror, shaderLighting, shaderFrameBuffer);
							}

							// Write pixel shader result to final output buffer. This only results in overdraw for ghosts.
							const uint8_t writtenPaletteIndex = g_paletteIndexBuffer[shaderFrameBuffer.pixelIndex];
							g_colorBuffer[shaderFrameBuffer.pixelIndex] = shaderFrameBuffer.palette.colors[writtenPaletteIndex];
							g_totalColorWrites++;
						}
					}
				}
			}
		}
	}

	template<RenderLightingType lightingType, PixelShaderType pixelShaderType, bool enableDepthRead, bool enableDepthWrite>
	void RasterizeMeshDispatchDitheringMode(int meshIndex, const SoftwareRenderer::ObjectTexturePool &textures)
	{
		switch (g_ditheringMode)
		{
		case DitheringMode::None:
			RasterizeMeshInternal<lightingType, pixelShaderType, enableDepthRead, enableDepthWrite, DitheringMode::None>(meshIndex, textures);
			break;
		case DitheringMode::Classic:
			RasterizeMeshInternal<lightingType, pixelShaderType, enableDepthRead, enableDepthWrite, DitheringMode::Classic>(meshIndex, textures);
			break;
		case DitheringMode::Modern:
			RasterizeMeshInternal<lightingType, pixelShaderType, enableDepthRead, enableDepthWrite, DitheringMode::Modern>(meshIndex, textures);
			break;
		}
	}

	template<RenderLightingType lightingType, PixelShaderType pixelShaderType>
	void RasterizeMeshDispatchDepthToggles(int meshIndex, const SoftwareRenderer::ObjectTexturePool &textures)
	{
		const bool enableDepthRead = g_meshProcessCaches.enableDepthReads[meshIndex];
		const bool enableDepthWrite = g_meshProcessCaches.enableDepthWrites[meshIndex];

		if (enableDepthRead)
		{
			if (enableDepthWrite)
			{
				RasterizeMeshDispatchDitheringMode<lightingType, pixelShaderType, true, true>(meshIndex, textures);
			}
			else
			{
				RasterizeMeshDispatchDitheringMode<lightingType, pixelShaderType, true, false>(meshIndex, textures);
			}
		}
		else
		{
			if (enableDepthWrite)
			{
				RasterizeMeshDispatchDitheringMode<lightingType, pixelShaderType, false, true>(meshIndex, textures);
			}
			else
			{
				RasterizeMeshDispatchDitheringMode<lightingType, pixelShaderType, false, false>(meshIndex, textures);
			}
		}
	}

	template<RenderLightingType lightingType>
	void RasterizeMeshDispatchPixelShaderType(int meshIndex, const SoftwareRenderer::ObjectTexturePool &textures)
	{
		static_assert(PixelShaderType::AlphaTestedWithHorizonMirror == PIXEL_SHADER_TYPE_MAX);
		const PixelShaderType pixelShaderType = g_meshProcessCaches.pixelShaderTypes[meshIndex];

		switch (pixelShaderType)
		{
		case PixelShaderType::Opaque:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::Opaque>(meshIndex, textures);
			break;
		case PixelShaderType::OpaqueWithAlphaTestLayer:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::OpaqueWithAlphaTestLayer>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTested:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTested>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithVariableTexCoordUMin:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithVariableTexCoordUMin>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithVariableTexCoordVMin:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithVariableTexCoordVMin>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithPaletteIndexLookup:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithPaletteIndexLookup>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithLightLevelColor:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithLightLevelColor>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithLightLevelOpacity:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithLightLevelOpacity>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithPreviousBrightnessLimit:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithPreviousBrightnessLimit>(meshIndex, textures);
			break;
		case PixelShaderType::AlphaTestedWithHorizonMirror:
			RasterizeMeshDispatchDepthToggles<lightingType, PixelShaderType::AlphaTestedWithHorizonMirror>(meshIndex, textures);
			break;
		}
	}

	// Decides which optimized rasterizer variant to use based on the parameters.
	void RasterizeMesh(int meshIndex, const SoftwareRenderer::ObjectTexturePool &textures)
	{
		static_assert(RenderLightingType::PerPixel == RENDER_LIGHTING_TYPE_MAX);
		const RenderLightingType lightingType = g_meshProcessCaches.lightingTypes[meshIndex];

		if (lightingType == RenderLightingType::PerMesh)
		{
			RasterizeMeshDispatchPixelShaderType<RenderLightingType::PerMesh>(meshIndex, textures);
		}
		else if (lightingType == RenderLightingType::PerPixel)
		{
			RasterizeMeshDispatchPixelShaderType<RenderLightingType::PerPixel>(meshIndex, textures);
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
	this->worldPointX = worldPoint.x;
	this->worldPointY = worldPoint.y;
	this->worldPointZ = worldPoint.z;
	this->startRadius = startRadius;
	this->endRadius = endRadius;
	this->startEndRadiusDiff = endRadius - startRadius;
	this->startEndRadiusDiffRecip = 1.0 / this->startEndRadiusDiff;
}

SoftwareRenderer::SoftwareRenderer()
{
	this->ditheringMode = static_cast<DitheringMode>(-1);
}

SoftwareRenderer::~SoftwareRenderer()
{

}

void SoftwareRenderer::init(const RenderInitSettings &settings)
{
	this->paletteIndexBuffer.init(settings.width, settings.height);
	this->depthBuffer.init(settings.width, settings.height);

	const DitheringMode ditheringMode = static_cast<DitheringMode>(settings.ditheringMode);
	CreateDitherBuffer(this->ditherBuffer, settings.width, settings.height, ditheringMode);
	this->ditheringMode = settings.ditheringMode;
}

void SoftwareRenderer::shutdown()
{
	this->paletteIndexBuffer.clear();
	this->depthBuffer.clear();
	this->ditherBuffer.clear();
	this->ditheringMode = static_cast<DitheringMode>(-1);
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
	light.worldPointX = worldPoint.x;
	light.worldPointY = worldPoint.y;
	light.worldPointZ = worldPoint.z;
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
	const int presentedTriangleCount = g_totalPresentedTriangleCount;

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

	return ProfilerData(renderWidth, renderHeight, threadCount, drawCallCount, presentedTriangleCount, textureCount,
		textureByteCount, totalLightCount, totalDepthTests, totalColorWrites);
}

void SoftwareRenderer::submitFrame(const RenderCamera &camera, BufferView<const RenderDrawCall> drawCalls,
	const RenderFrameSettings &settings, uint32_t *outputBuffer)
{
	const int frameBufferWidth = this->paletteIndexBuffer.getWidth();
	const int frameBufferHeight = this->paletteIndexBuffer.getHeight();

	if (this->ditheringMode != settings.ditheringMode)
	{
		this->ditheringMode = settings.ditheringMode;
		CreateDitherBuffer(this->ditherBuffer, frameBufferWidth, frameBufferHeight, settings.ditheringMode);
	}

	const ObjectTexture &paletteTexture = this->objectTextures.get(settings.paletteTextureID);
	const ObjectTexture &lightTableTexture = this->objectTextures.get(settings.lightTableTextureID);
	const ObjectTexture &skyBgTexture = this->objectTextures.get(settings.skyBgTextureID);

	PopulateCameraGlobals(camera);
	PopulateRasterizerGlobals(frameBufferWidth, frameBufferHeight, this->paletteIndexBuffer.begin(), this->depthBuffer.begin(),
		this->ditherBuffer.begin(), this->ditherBuffer.getDepth(), this->ditheringMode, outputBuffer);
	PopulatePixelShaderGlobals(settings.ambientPercent, paletteTexture, lightTableTexture, skyBgTexture);

	ClearTriangleTotalCounts();
	ClearFrameBuffers();

	const RenderDrawCall *drawCallsPtr = drawCalls.begin();
	const int drawCallCount = drawCalls.getCount();
	g_totalDrawCallCount = drawCallCount;

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
			PopulateMeshTransform(sequenceIndex, transform);

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
		ProcessClipSpaceTrianglesForFrontFacing(drawCallSequenceCount);

		for (int meshIndex = 0; meshIndex < drawCallSequenceCount; meshIndex++)
		{
			RasterizeMesh(meshIndex, this->objectTextures);
		}

		drawCallIndex += drawCallSequenceCount;
	}
}

void SoftwareRenderer::present()
{
	// Do nothing for now, might change later.
}
