// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace StampShapes
{

	bool Initialize();

	void Release();

	void Retry();

	bool Ready();

	ID3D11ShaderResourceView* View();

	void Analyse();

	void RequestAnalysis();
}
