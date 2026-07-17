// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Noises/PCGExNoiseCurl.h"
#include "Containers/PCGExManagedObjects.h"
#include "Helpers/PCGExNoise3DMath.h"

using namespace PCGExNoise3D::Math;

namespace PCGExNoiseCurl
{
	// Noise-space offsets decorrelating the three potential-field channels
	const FVector OffsetFy = FVector(31.416, 47.853, 12.793);
	const FVector OffsetFz = FVector(93.139, 25.186, 71.524);
}

double FPCGExNoiseCurl::BaseNoise(const FVector& Position) const
{
	return Perlin3D(Position, Seed);
}

FVector FPCGExNoiseCurl::GetPotentialField(const FVector& Position) const
{
	// Three independent noise channels for potential field
	const FVector ScaledPos = Position * Frequency;
	const double Fx = BaseNoise(ScaledPos);
	const double Fy = BaseNoise(ScaledPos + PCGExNoiseCurl::OffsetFy);
	const double Fz = BaseNoise(ScaledPos + PCGExNoiseCurl::OffsetFz);
	return FVector(Fx, Fy, Fz);
}

FVector FPCGExNoiseCurl::ComputeCurl(const FVector& Position) const
{
	const double E = Epsilon;
	const double InvE2 = 1.0 / (2.0 * E);

	// Central differences for partial derivatives
	const FVector PxP = GetPotentialField(Position + FVector(E, 0, 0));
	const FVector PxN = GetPotentialField(Position - FVector(E, 0, 0));
	const FVector PyP = GetPotentialField(Position + FVector(0, E, 0));
	const FVector PyN = GetPotentialField(Position - FVector(0, E, 0));
	const FVector PzP = GetPotentialField(Position + FVector(0, 0, E));
	const FVector PzN = GetPotentialField(Position - FVector(0, 0, E));

	// Partial derivatives
	const double dFz_dy = (PyP.Z - PyN.Z) * InvE2;
	const double dFy_dz = (PzP.Y - PzN.Y) * InvE2;
	const double dFx_dz = (PzP.X - PzN.X) * InvE2;
	const double dFz_dx = (PxP.Z - PxN.Z) * InvE2;
	const double dFy_dx = (PxP.Y - PxN.Y) * InvE2;
	const double dFx_dy = (PyP.X - PyN.X) * InvE2;

	// Curl: (dFz/dy - dFy/dz, dFx/dz - dFz/dx, dFy/dx - dFx/dy)
	return FVector(
		dFz_dy - dFy_dz,
		dFx_dz - dFz_dx,
		dFy_dx - dFx_dy
		) * CurlScale;
}

double FPCGExNoiseCurl::ComputeCurlX(const FVector& Position) const
{
	// X component only (dFz/dy - dFy/dz): 4 base samples instead of ComputeCurl's 18
	const double E = Epsilon;
	const double InvE2 = 1.0 / (2.0 * E);

	const double FzYP = BaseNoise((Position + FVector(0, E, 0)) * Frequency + PCGExNoiseCurl::OffsetFz);
	const double FzYN = BaseNoise((Position - FVector(0, E, 0)) * Frequency + PCGExNoiseCurl::OffsetFz);
	const double FyZP = BaseNoise((Position + FVector(0, 0, E)) * Frequency + PCGExNoiseCurl::OffsetFy);
	const double FyZN = BaseNoise((Position - FVector(0, 0, E)) * Frequency + PCGExNoiseCurl::OffsetFy);

	const double dFz_dy = (FzYP - FzYN) * InvE2;
	const double dFy_dz = (FyZP - FyZN) * InvE2;

	return (dFz_dy - dFy_dz) * CurlScale;
}

double FPCGExNoiseCurl::ComputeCurlY(const FVector& Position) const
{
	// Y component only (dFx/dz - dFz/dx)
	const double E = Epsilon;
	const double InvE2 = 1.0 / (2.0 * E);

	const double FxZP = BaseNoise((Position + FVector(0, 0, E)) * Frequency);
	const double FxZN = BaseNoise((Position - FVector(0, 0, E)) * Frequency);
	const double FzXP = BaseNoise((Position + FVector(E, 0, 0)) * Frequency + PCGExNoiseCurl::OffsetFz);
	const double FzXN = BaseNoise((Position - FVector(E, 0, 0)) * Frequency + PCGExNoiseCurl::OffsetFz);

	const double dFx_dz = (FxZP - FxZN) * InvE2;
	const double dFz_dx = (FzXP - FzXN) * InvE2;

	return (dFx_dz - dFz_dx) * CurlScale;
}

double FPCGExNoiseCurl::GetDouble(const FVector& Position) const
{
	double CurlX = ComputeCurlX(TransformPosition(Position));

	if (Octaves > 1)
	{
		double Amp = 1.0;
		double Freq = 1.0;

		for (int32 i = 1; i < Octaves; ++i)
		{
			Amp *= Persistence;
			Freq *= Lacunarity;
			CurlX += ComputeCurlX(Position * Freq) * Amp;
		}

		CurlX *= FractalBounding;
	}

	return ApplyRemap(CurlX * 0.5 + 0.5);
}

FVector2D FPCGExNoiseCurl::GetVector2D(const FVector& Position) const
{
	const FVector Transformed = TransformPosition(Position);
	double CurlX = ComputeCurlX(Transformed);
	double CurlY = ComputeCurlY(Transformed);

	if (Octaves > 1)
	{
		double Amp = 1.0;
		double Freq = 1.0;

		for (int32 i = 1; i < Octaves; ++i)
		{
			Amp *= Persistence;
			Freq *= Lacunarity;
			CurlX += ComputeCurlX(Position * Freq) * Amp;
			CurlY += ComputeCurlY(Position * Freq) * Amp;
		}

		CurlX *= FractalBounding;
		CurlY *= FractalBounding;
	}

	return FVector2D(
		ApplyRemap(CurlX * 0.5 + 0.5),
		ApplyRemap(CurlY * 0.5 + 0.5)
		);
}

FVector FPCGExNoiseCurl::GetVector(const FVector& Position) const
{
	FVector Curl = ComputeCurl(TransformPosition(Position));

	// Apply fractal octaves if configured
	if (Octaves > 1)
	{
		double Amp = 1.0;
		double Freq = 1.0;

		for (int32 i = 1; i < Octaves; ++i)
		{
			Amp *= Persistence;
			Freq *= Lacunarity;
			Curl += ComputeCurl(Position * Freq) * Amp;
		}

		Curl *= FractalBounding;
	}

	for (int i = 0; i < 3; i++)
	{
		Curl[i] = ApplyRemap(Curl[i] * 0.5 + 0.5);
	}

	return Curl;
}

FVector4 FPCGExNoiseCurl::GetVector4(const FVector& Position) const
{
	const FVector Curl = GetVector(Position);
	return FVector4(Curl.X, Curl.Y, Curl.Z, Curl.Size());
}

TSharedPtr<FPCGExNoise3DOperation> UPCGExNoise3DFactoryCurl::CreateOperation(FPCGExContext* InContext) const
{
	PCGEX_FACTORY_NEW_OPERATION(NoiseCurl)
	PCGEX_FORWARD_NOISE3D_CONFIG

	NewOperation->Octaves = Config.Octaves;
	NewOperation->Lacunarity = Config.Lacunarity;
	NewOperation->Persistence = Config.Persistence;
	NewOperation->Epsilon = Config.Epsilon;
	NewOperation->CurlScale = Config.CurlScale;

	return NewOperation;
}

PCGEX_NOISE3D_FACTORY_BOILERPLATE_IMPL(Curl, {})

UPCGExFactoryData* UPCGExNoise3DCurlProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExNoise3DFactoryCurl* NewFactory = InContext->ManagedObjects->New<UPCGExNoise3DFactoryCurl>();
	PCGEX_FORWARD_NOISE3D_FACTORY
	return Super::CreateFactory(InContext, NewFactory);
}
