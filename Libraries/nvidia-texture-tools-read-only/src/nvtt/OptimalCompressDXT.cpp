// Copyright NVIDIA Corporation 2007 -- Ignacio Castano <icastano@nvidia.com>
// 
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include "OptimalCompressDXT.h"
#include "SingleColorLookup.h"

#include <nvimage/ColorBlock.h>
#include <nvimage/BlockDXT.h>

#include <nvmath/Color.h>

#include <nvcore/Containers.h> // swap

#include <limits.h>

using namespace nv;
using namespace OptimalCompress;



namespace
{
	static int greenDistance(int g0, int g1)
	{
		//return abs(g0 - g1);
		int d = g0 - g1;
		return d * d;
	}

	static int alphaDistance(int a0, int a1)
	{
		//return abs(a0 - a1);
		int d = a0 - a1;
		return d * d;
	}

	static uint nearestGreen4(uint green, uint maxGreen, uint minGreen)
	{
		uint bias = maxGreen + (maxGreen - minGreen) / 6;

		uint index = 0;
		if (maxGreen - minGreen != 0) index = clamp(3 * (bias - green) / (maxGreen - minGreen), 0U, 3U);

		return (index * minGreen + (3 - index) * maxGreen) / 3;
	}

	static int computeGreenError(const ColorBlock & rgba, const BlockDXT1 * block, int bestError = INT_MAX)
	{
		nvDebugCheck(block != NULL);

	//	uint g0 = (block->col0.g << 2) | (block->col0.g >> 4);
	//	uint g1 = (block->col1.g << 2) | (block->col1.g >> 4);

		int palette[4];
		palette[0] = (block->col0.g << 2) | (block->col0.g >> 4);
		palette[1] = (block->col1.g << 2) | (block->col1.g >> 4);
		palette[2] = (2 * palette[0] + palette[1]) / 3;
		palette[3] = (2 * palette[1] + palette[0]) / 3;

		int totalError = 0;
		for (int i = 0; i < 16; i++)
		{
			const int green = rgba.color(i).g;
			
			int error = greenDistance(green, palette[0]);
			error = min(error, greenDistance(green, palette[1]));
			error = min(error, greenDistance(green, palette[2]));
			error = min(error, greenDistance(green, palette[3]));

			totalError += error;

		//	totalError += nearestGreen4(green, g0, g1);

			if (totalError > bestError)
			{
				// early out
				return totalError;
			}
		}

		return totalError;
	}

	static uint computeGreenIndices(const ColorBlock & rgba, const Color32 palette[4])
	{
		const int color0 = palette[0].g;
		const int color1 = palette[1].g;
		const int color2 = palette[2].g;
		const int color3 = palette[3].g;
		
		uint indices = 0;
		for (int i = 0; i < 16; i++)
		{
			const int color = rgba.color(i).g;
			
			uint d0 = greenDistance(color0, color);
			uint d1 = greenDistance(color1, color);
			uint d2 = greenDistance(color2, color);
			uint d3 = greenDistance(color3, color);
			
			uint b0 = d0 > d3;
			uint b1 = d1 > d2;
			uint b2 = d0 > d2;
			uint b3 = d1 > d3;
			uint b4 = d2 > d3;
			
			uint x0 = b1 & b2;
			uint x1 = b0 & b3;
			uint x2 = b0 & b4;
			
			indices |= (x2 | ((x0 | x1) << 1)) << (2 * i);
		}

		return indices;
	}

	// Choose quantized color that produces less error. Used by DXT3 compressor.
	inline static uint quantize4(uint8 a)
	{
		int q0 = max(int(a >> 4) - 1, 0);
		int q1 = (a >> 4);
		int q2 = min(int(a >> 4) + 1, 0xF);
		
		q0 = (q0 << 4) | q0;
		q1 = (q1 << 4) | q1;
		q2 = (q2 << 4) | q2;
		
		int d0 = alphaDistance(q0, a);
		int d1 = alphaDistance(q1, a);
		int d2 = alphaDistance(q2, a);

		if (d0 < d1 && d0 < d2) return q0 >> 4;
		if (d1 < d2) return q1 >> 4;
		return q2 >> 4;
	}
	
	static uint nearestAlpha8(uint alpha, uint maxAlpha, uint minAlpha)
	{
		float bias = maxAlpha + float(maxAlpha - minAlpha) / (2.0f * 7.0f);
		float scale = 7.0f / float(maxAlpha - minAlpha);

		uint index = (uint)clamp((bias - float(alpha)) * scale, 0.0f, 7.0f);

		return (index * minAlpha + (7 - index) * maxAlpha) / 7;
	}

	static uint computeAlphaError8(const ColorBlock & rgba, const AlphaBlockDXT5 * block, int bestError = INT_MAX)
	{
		int totalError = 0;

		for (uint i = 0; i < 16; i++)
		{
			uint8 alpha = rgba.color(i).a;

			totalError += alphaDistance(alpha, nearestAlpha8(alpha, block->alpha0, block->alpha1));

			if (totalError > bestError)
			{
				// early out
				return totalError;
			}
		}

		return totalError;
	}

	static uint computeAlphaError(const ColorBlock & rgba, const AlphaBlockDXT5 * block, int bestError = INT_MAX)
	{
		uint8 alphas[8];
		block->evaluatePalette(alphas);

		int totalError = 0;

		for (uint i = 0; i < 16; i++)
		{
			uint8 alpha = rgba.color(i).a;

			int minDist = INT_MAX;
			for (uint p = 0; p < 8; p++)
			{
				int dist = alphaDistance(alpha, alphas[p]);
				minDist = min(dist, minDist);
			}

			totalError += minDist;

			if (totalError > bestError)
			{
				// early out
				return totalError;
			}
		}

		return totalError;
	}
	
	static void computeAlphaIndices(const ColorBlock & rgba, AlphaBlockDXT5 * block)
	{
		uint8 alphas[8];
		block->evaluatePalette(alphas);

		for (uint i = 0; i < 16; i++)
		{
			uint8 alpha = rgba.color(i).a;

			int minDist = INT_MAX;
			int bestIndex = 8;
			for (uint p = 0; p < 8; p++)
			{
				int dist = alphaDistance(alpha, alphas[p]);

				if (dist < minDist)
				{
					minDist = dist;
					bestIndex = p;
				}
			}
			nvDebugCheck(bestIndex < 8);

			block->setIndex(i, bestIndex);
		}
	}

} // namespace





// Single color compressor, based on:
// https://mollyrocket.com/forums/viewtopic.php?t=392
void OptimalCompress::compressDXT1(Color32 c, BlockDXT1 * dxtBlock)
{
	dxtBlock->col0.r = OMatch5[c.r][0];
	dxtBlock->col0.g = OMatch6[c.g][0];
	dxtBlock->col0.b = OMatch5[c.b][0];
	dxtBlock->col1.r = OMatch5[c.r][1];
	dxtBlock->col1.g = OMatch6[c.g][1];
	dxtBlock->col1.b = OMatch5[c.b][1];
	dxtBlock->indices = 0xaaaaaaaa;

	if (dxtBlock->col0.u < dxtBlock->col1.u)
	{
		swap(dxtBlock->col0.u, dxtBlock->col1.u);
		dxtBlock->indices ^= 0x55555555;
	}
}

void OptimalCompress::compressDXT1a(Color32 rgba, BlockDXT1 * dxtBlock)
{
	if (rgba.a < 128)
	{
		dxtBlock->col0.u = 0;
		dxtBlock->col1.u = 0;
		dxtBlock->indices = 0xFFFFFFFF;
	}
	else
	{
		compressDXT1(rgba, dxtBlock);
	}
}

void OptimalCompress::compressDXT1G(uint8 g, BlockDXT1 * dxtBlock)
{
	dxtBlock->col0.r = 31;
	dxtBlock->col0.g = OMatch6[g][0];
	dxtBlock->col0.b = 0;
	dxtBlock->col1.r = 31;
	dxtBlock->col1.g = OMatch6[g][1];
	dxtBlock->col1.b = 0;
	dxtBlock->indices = 0xaaaaaaaa;

	if (dxtBlock->col0.u < dxtBlock->col1.u)
	{
		swap(dxtBlock->col0.u, dxtBlock->col1.u);
		dxtBlock->indices ^= 0x55555555;
	}
}


// Brute force green channel compressor
void OptimalCompress::compressDXT1G(const ColorBlock & rgba, BlockDXT1 * block)
{
	nvDebugCheck(block != NULL);
	
	uint8 ming = 63;
	uint8 maxg = 0;
	
	bool isSingleColor = true;
	uint8 singleColor = rgba.color(0).g;

	// Get min/max green.
	for (uint i = 0; i < 16; i++)
	{
		uint8 green = (rgba.color(i).g + 1) >> 2;
		ming = min(ming, green);
		maxg = max(maxg, green);

		if (rgba.color(i).g != singleColor) isSingleColor = false;
	}

	if (isSingleColor)
	{
		compressDXT1G(singleColor, block);
		return;
	}

	block->col0.r = 31;
	block->col1.r = 31;
	block->col0.g = maxg;
	block->col1.g = ming;
	block->col0.b = 0;
	block->col1.b = 0;

	int bestError = computeGreenError(rgba, block);
	int bestg0 = maxg;
	int bestg1 = ming;

	// Expand search space a bit.
	const int greenExpand = 4;
	ming = (ming <= greenExpand) ? 0 : ming - greenExpand;
	maxg = (maxg >= 63-greenExpand) ? 63 : maxg + greenExpand;

	for (int g0 = ming+1; g0 <= maxg; g0++)
	{
		for (int g1 = ming; g1 < g0; g1++)
		{
			block->col0.g = g0;
			block->col1.g = g1;
			int error = computeGreenError(rgba, block, bestError);
			
			if (error < bestError)
			{
				bestError = error;
				bestg0 = g0;
				bestg1 = g1;
			}
		}
	}
	
	block->col0.g = bestg0;
	block->col1.g = bestg1;

	nvDebugCheck(bestg0 == bestg1 || block->isFourColorMode());


	Color32 palette[4];
	block->evaluatePalette(palette);
	block->indices = computeGreenIndices(rgba, palette);
}

void OptimalCompress::compressDXT3A(const ColorBlock & rgba, AlphaBlockDXT3 * dxtBlock)
{
	dxtBlock->alpha0 = quantize4(rgba.color(0).a);
	dxtBlock->alpha1 = quantize4(rgba.color(1).a);
	dxtBlock->alpha2 = quantize4(rgba.color(2).a);
	dxtBlock->alpha3 = quantize4(rgba.color(3).a);
	dxtBlock->alpha4 = quantize4(rgba.color(4).a);
	dxtBlock->alpha5 = quantize4(rgba.color(5).a);
	dxtBlock->alpha6 = quantize4(rgba.color(6).a);
	dxtBlock->alpha7 = quantize4(rgba.color(7).a);
	dxtBlock->alpha8 = quantize4(rgba.color(8).a);
	dxtBlock->alpha9 = quantize4(rgba.color(9).a);
	dxtBlock->alphaA = quantize4(rgba.color(10).a);
	dxtBlock->alphaB = quantize4(rgba.color(11).a);
	dxtBlock->alphaC = quantize4(rgba.color(12).a);
	dxtBlock->alphaD = quantize4(rgba.color(13).a);
	dxtBlock->alphaE = quantize4(rgba.color(14).a);
	dxtBlock->alphaF = quantize4(rgba.color(15).a);
}


void OptimalCompress::compressDXT5A(const ColorBlock & rgba, AlphaBlockDXT5 * dxtBlock)
{
	uint8 mina = 255;
	uint8 maxa = 0;

	// Get min/max alpha.
	for (uint i = 0; i < 16; i++)
	{
		uint8 alpha = rgba.color(i).a;
		mina = min(mina, alpha);
		maxa = max(maxa, alpha);
	}

	dxtBlock->alpha0 = maxa;
	dxtBlock->alpha1 = mina;

	if (maxa - mina > 8)
	{
		int besterror = computeAlphaError(rgba, dxtBlock);
		int besta0 = maxa;
		int besta1 = mina;

		// Expand search space a bit.
		const int alphaExpand = 8;
		mina = (mina <= alphaExpand) ? 0 : mina - alphaExpand;
		maxa = (maxa >= 255-alphaExpand) ? 255 : maxa + alphaExpand;

		for (int a0 = mina+9; a0 < maxa; a0++)
		{
			for (int a1 = mina; a1 < a0-8; a1++)
			{
				nvDebugCheck(a0 - a1 > 8);

				dxtBlock->alpha0 = a0;
				dxtBlock->alpha1 = a1;
				int error = computeAlphaError(rgba, dxtBlock, besterror);

				if (error < besterror)
				{
					besterror = error;
					besta0 = a0;
					besta1 = a1;
				}
			}
		}

		dxtBlock->alpha0 = besta0;
		dxtBlock->alpha1 = besta1;
	}

	computeAlphaIndices(rgba, dxtBlock);
}

