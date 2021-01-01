/**
 * Copyright (C) 2019 Francesco Fusco. All rights reserved.
 * License: https://github.com/Fushko/gammy#license
 */

#include "dspctl-dxgi.h"
#include "defs.h"
#include "cfg.h"
#include "utils.h"

DspCtl::DspCtl()
{
	GDI::getDisplays();
	if (GDI::hdcs.empty()) {
		LOGF << "No GDI HDCs detected.";
		exit(EXIT_FAILURE);
	}

	useDXGI = init();
	LOGD << "DXGI available: " << useDXGI;
}

void DspCtl::getSnapshot(std::vector<uint8_t> &buf) noexcept
{
	if (useDXGI) {
		while (!this->getFrame(buf))
			restart();
	} else {
		GDI::getSnapshot(buf);
	}
}

/**
 * DXGI Gamma control works only in fullscreen.
 * We can only use GDI.
 */
void DspCtl::setGamma(int brt_step, int temp_step)
{
	GDI::setGamma(brt_step, temp_step);
}

bool DspCtl::init()
{
	IDXGIOutput                 *output;
	IDXGIAdapter1               *pAdapter;
	std::vector<IDXGIOutput*>   vOutputs;   // Monitors vector
	std::vector<IDXGIAdapter1*> vAdapters;  // GPUs vector

	// Retrieve a IDXGIFactory to enumerate the adapters
	{
		IDXGIFactory1 *pFactory = nullptr;
		HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory));

		if (hr != S_OK) {
			LOGE << "Failed to retrieve the IDXGIFactory";
			return false;
		}

		UINT i = 0;

		while (pFactory->EnumAdapters1(i++, &pAdapter) != DXGI_ERROR_NOT_FOUND)
			vAdapters.push_back(pAdapter);

		pFactory->Release();

		IF_PLOG(plog::debug) { // Get GPU info
			DXGI_ADAPTER_DESC1 desc;
			for (UINT i = 0; i < vAdapters.size(); ++i) {
				pAdapter = vAdapters[i];
				HRESULT hr = pAdapter->GetDesc1(&desc);
				if (hr != S_OK) {
					LOGE << "Failed to get description for adapter: " << i;
					continue;
				}
				LOGD << "Adapter " << i << ": " << desc.Description;
			}
		}
	}

	// Get the monitors attached to the GPUs
	{
		UINT j;

		for (UINT i = 0; i < vAdapters.size(); ++i) {
			j = 0;
			pAdapter = vAdapters[i];
			while (pAdapter->EnumOutputs(j++, &output) != DXGI_ERROR_NOT_FOUND) {
				LOGD << "Found monitor " << j << " on adapter " << i;
				vOutputs.push_back(output);
			}
		}

		if (vOutputs.empty()) {
			LOGE << "No outputs found";
			return false;
		}

		// Print monitor info
		DXGI_OUTPUT_DESC desc;

		for (size_t i = 0; i < vOutputs.size(); ++i) {
			output = vOutputs[i];
			HRESULT hr = output->GetDesc(&desc);
			if (hr != S_OK) {
				LOGE << "Failed to get description for output " << i;
				continue;
			}
			LOGD << "Monitor: " << desc.DeviceName << ", attached to desktop: " << desc.AttachedToDesktop;
		}
	}

	// Create a Direct3D device to access the OutputDuplication interface
	{
		D3D_FEATURE_LEVEL d3d_feature_level;
		IDXGIAdapter1 *d3d_adapter;
		UINT use_adapter = 0;

		if (vAdapters.size() <= use_adapter) {
			LOGE << "Invalid adapter index: " << use_adapter << ", we only have: " << vAdapters.size();
			return false;
		}

		d3d_adapter = vAdapters[use_adapter];

		if (!d3d_adapter) {
			LOGE << "The stored adapter is nullptr";
			return false;
		}

		HRESULT hr = D3D11CreateDevice(
		                        d3d_adapter,              // Adapter: The adapter (video card) we want to use. We may use NULL to pick the default adapter.
		                        D3D_DRIVER_TYPE_UNKNOWN,  // DriverType: We use the GPU as backing device.
		                        nullptr,                  // Software: we're using a D3D_DRIVER_TYPE_HARDWARE so it's not applicable.
		                        NULL,                     // Flags: maybe we need to use D3D11_CREATE_DEVICE_BGRA_SUPPORT because desktop duplication is using this.
		                        nullptr,                  // Feature Levels:  what version to use.
		                        0,                        // Number of feature levels.
		                        D3D11_SDK_VERSION,        // The SDK version, use D3D11_SDK_VERSION
		                        &d3d_device,              // OUT: the ID3D11Device object.
		                        &d3d_feature_level,       // OUT: the selected feature level.
		                        &d3d_context);            // OUT: the ID3D11DeviceContext that represents the above features.

		d3d_context->Release();
		d3d_adapter->Release();

		if (hr != S_OK) {
			LOGE << "Failed to create D3D11 Device.";
			if (hr == E_INVALIDARG) {
				LOGE << "Got INVALID arg passed into D3D11CreateDevice. Did you pass a adapter + a driver which is not the UNKNOWN driver?";
			}
			return false;
		}
	}

	// Choose what monitor to use
	{
		UINT use_monitor = 0;
		output = vOutputs[use_monitor];

		if (use_monitor >= vOutputs.size()) {
			LOGE << "Invalid monitor index";
			return false;
		}

		if (!output) {
			LOGE << "No valid output found. The output is nullptr";
			return false;
		}
	}

	// Set texture properties
	{
		DXGI_OUTPUT_DESC desc;
		HRESULT hr = output->GetDesc(&desc);

		if (hr != S_OK) {
			LOGE << "Failed to get output description";
			return false;
		}

		tex_desc.Width = desc.DesktopCoordinates.right;
		tex_desc.Height = desc.DesktopCoordinates.bottom;
		tex_desc.MipLevels = 1;
		tex_desc.ArraySize = 1;
		tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		tex_desc.SampleDesc.Count = 1;
		tex_desc.SampleDesc.Quality = 0;
		tex_desc.Usage = D3D11_USAGE_STAGING;
		tex_desc.BindFlags = 0;
		tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		tex_desc.MiscFlags = 0;
	}

	// Initialize output duplication
	{
		HRESULT hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));

		if (hr != S_OK) {
			LOGE << "Failed to query IDXGIOutput1 interface";
			return false;
		}

		hr = output1->DuplicateOutput(d3d_device, &duplication);

		if (hr != S_OK) {
			LOGE << "DuplicateOutput failed";
			return false;
		}

		output1->Release();
		d3d_device->Release();
	}

	for (auto adapter : vAdapters)
		adapter->Release();

	for (auto output : vOutputs)
		output->Release();

	return true;
}

bool DspCtl::getFrame(std::vector<uint8_t> &buf)
{
	HRESULT hr;

	ID3D11Texture2D *tex;
	DXGI_OUTDUPL_FRAME_INFO frame_info;
	IDXGIResource *desktop_resource;

	LOGV << "Acquiring frame";
	hr = duplication->AcquireNextFrame(INFINITE, &frame_info, &desktop_resource);
	switch (hr) {
	case S_OK: {
		// Get the texture interface
		hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));

		desktop_resource->Release();
		tex->Release();

		if (hr != S_OK) {
			LOGE << "Failed to query the ID3D11Texture2D interface on the IDXGIResource";
			return false;
		}

		break;
	}
	case DXGI_ERROR_ACCESS_LOST:
		LOGE << "Received a DXGI_ERROR_ACCESS_LOST";
		return false;
	case DXGI_ERROR_WAIT_TIMEOUT:
		LOGE << "Received a DXGI_ERROR_WAIT_TIMEOUT";
		return false;
	default:
		LOGE << "Error: failed to acquire frame";
		return false;
	}

	ID3D11Texture2D *staging_tex;
	hr = d3d_device->CreateTexture2D(&tex_desc, nullptr, &staging_tex);

	if (hr != S_OK) {
		LOGE << "Texture creation failed, error: " << hr;
		return false;
	}

	d3d_context->CopyResource(staging_tex, tex);
	duplication->ReleaseFrame();

	D3D11_MAPPED_SUBRESOURCE map;

	do {
		Sleep(cfg["brt_polling_rate"].get<int>());
	} while (d3d_context->Map(staging_tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) == DXGI_ERROR_WAS_STILL_DRAWING);

	d3d_context->Unmap(staging_tex, 0);
	staging_tex->Release();
	d3d_context->Release();

	LOGV << "D31D11 map row pitch: " << map.RowPitch << ", depth pitch: " << map.DepthPitch;
	uint8_t *x = reinterpret_cast<uint8_t*>(map.pData);
	LOGV << "Copying buffer";
	buf.assign(x, x + map.DepthPitch);

	return true;
}

void DspCtl::restart()
{
	HRESULT hr;

	do {
		hr = output1->DuplicateOutput(d3d_device, &duplication);

		IF_PLOG(plog::debug)
		{
			if (hr != S_OK) {
				LOGE << "** Unable to duplicate output. Reason:";

				switch (hr)
				{
				case E_INVALIDARG:                          LOGE << "E_INVALIDARG"; break;
				case E_ACCESSDENIED:                        LOGE << "E_ACCESSDENIED"; break;
				case DXGI_ERROR_UNSUPPORTED:                LOGE << "E_DXGI_ERROR_UNSUPPORTED"; break;
				case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:    LOGE << "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE"; break;
				case DXGI_ERROR_SESSION_DISCONNECTED:       LOGE << "DXGI_ERROR_SESSION_DISCONNECTED"; break;
				}

				LOGD << "Retrying... (2.5 sec)";
			}
		}
		Sleep(2500);
	} while (hr != S_OK);

	LOGI << "Output duplication restarted.";

	output1->Release();
}

void DspCtl::setInitialGamma([[maybe_unused]]bool prev_gamma)
{
	setGamma(brt_slider_steps, temp_slider_steps);
}

DspCtl::~DspCtl()
{
	if (duplication)
		duplication->ReleaseFrame();
	if (output1)
		output1->Release();
	if (d3d_context)
		d3d_context->Release();
	if (d3d_device)
		d3d_device->Release();
}

// GDI ------------------------------------------------------------------------

void GDI::getSnapshot(std::vector<uint8_t> &buf)
{
	BITMAPINFOHEADER info;
	ZeroMemory(&info, sizeof(BITMAPINFOHEADER));
	info.biSize = sizeof(BITMAPINFOHEADER);
	info.biWidth = w;
	info.biHeight = -h;
	info.biPlanes = 1;
	info.biBitCount = 32;
	info.biCompression = BI_RGB;
	info.biSizeImage = w * h * 4;
	info.biClrUsed = 0;
	info.biClrImportant = 0;

	HDC     dc     = GetDC(NULL);
	HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
	HDC     tmp    = CreateCompatibleDC(dc);
	HGDIOBJ obj    = SelectObject(tmp, bitmap);

	BitBlt(tmp, 0, 0, w, h, dc, 0, 0, SRCCOPY);
	GetDIBits(tmp, bitmap, 0, h, buf.data(), LPBITMAPINFO(&info), DIB_RGB_COLORS);

	SelectObject(tmp, obj);
	DeleteObject(bitmap);
	DeleteObject(obj);
	DeleteDC(dc);
}

void GDI::setGammaOld(int brt_step, int temp_step)
{
	if (brt_step > brt_slider_steps) {
		return;
	}

	const double r_mult = interpTemp(temp_step, 0),
	             g_mult = interpTemp(temp_step, 1),
	             b_mult = interpTemp(temp_step, 2);

	WORD ramp[3][256];

	const auto brt_mult = remap(brt_step, 0, brt_slider_steps, 0, 255);

	for (WORD i = 0; i < 256; ++i) {
		const int val = i * brt_mult;
		ramp[0][i] = WORD(val * r_mult);
		ramp[1][i] = WORD(val * g_mult);
		ramp[2][i] = WORD(val * b_mult);
	}

	HDC dc = GetDC(nullptr);
	BOOL r = SetDeviceGammaRamp(dc, ramp);
	LOGD << "Gamma set: " << r;
	ReleaseDC(nullptr, dc);
}

int GDI::numDisplays()
{
	DISPLAY_DEVICE dsp;
	dsp.cb = sizeof(DISPLAY_DEVICE);

	int i = 0;
	int attached_dsp = 0;

	while (EnumDisplayDevices(NULL, i++, &dsp, 0) && dsp.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
		attached_dsp++;
	}

	return attached_dsp;
}

void GDI::getDisplays()
{
	const int num_dsp = GDI::numDisplays();
	GDI::hdcs.reserve(num_dsp);

	for (int i = 0; i < num_dsp; ++i) {
		DISPLAY_DEVICE dsp;
		dsp.cb = sizeof(DISPLAY_DEVICE);
		EnumDisplayDevices(NULL, i, &dsp, 0);
		GDI::hdcs.push_back(CreateDC(NULL, dsp.DeviceName, NULL, 0));
	}

	LOGD << "HDCs: " << GDI::hdcs.size();
}

void GDI::setGamma(int brt_step, int temp_step)
{
	const double r_mult = interpTemp(temp_step, 0),
	             g_mult = interpTemp(temp_step, 1),
	             b_mult = interpTemp(temp_step, 2);

	WORD ramp[3][256];

	const auto brt_mult = remap(brt_step, 0, brt_slider_steps, 0, 255);

	for (WORD i = 0; i < 256; ++i) {
		const int val = i * brt_mult;
		ramp[0][i] = WORD(val * r_mult);
		ramp[1][i] = WORD(val * g_mult);
		ramp[2][i] = WORD(val * b_mult);
	}

	int i = 0;
	for (auto &dc : hdcs) {
		BOOL r = SetDeviceGammaRamp(dc, ramp);
		LOGV << "screen " << i++ <<  " gamma set: " << r;
	}
}