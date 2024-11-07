#pragma once

#ifdef _WIN64
#include <d2d1_3.h>
#include <shcore.h>
#include <Windows.Storage.h>
#include <Windows.Storage.Streams.h>
#include <Windows.Data.Pdf.h>
#include <windows.data.pdf.interop.h>
#include <wincodec.h>
#include <wrl.h>
#include "Win78Libraries.h"
#endif

#include "image.hpp"
#include <shlwapi.h>
#include <gdiplus.h>
#include <filesystem>

#pragma comment(lib, "shlwapi")
#pragma comment(lib, "gdiplus") 

struct ImageRenderer
{
	virtual ~ImageRenderer() {};
	virtual bool load(const wchar_t *filename) = 0;
	virtual bool isValid() const = 0;
	virtual void render(Image& img, int page, float zoom) = 0;
	virtual unsigned getPageCount() const = 0;
};

#ifdef _WIN64

class PdfRenderer: public ImageRenderer
{
public:
	virtual ~PdfRenderer()
	{
		if (s_dwThreadId)
		{
			PdfRendererThreadParams params{};
			Microsoft::WRL::Wrappers::Event unloadCompleted(CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, WRITE_OWNER | EVENT_ALL_ACCESS));
			params.hEvent = unloadCompleted.Get();
			params.type = PdfRendererThreadParams::Close;
			params.result = false;
			params.docId = m_docId;
			if (!PostThreadMessage(s_dwThreadId, WM_USER, 0, reinterpret_cast<LPARAM>(&params)))
				return;
			WaitForSingleObject(params.hEvent, INFINITE);
		}
	}

	virtual bool isValid() const override
	{
		return s_thread.IsValid();
	}

	virtual bool load(const wchar_t *filename) override
	{
		Microsoft::WRL::Wrappers::Event loadCompleted(CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, WRITE_OWNER | EVENT_ALL_ACCESS));

		PdfRendererThreadParams params{};
		params.filename = filename;
		params.hEvent = loadCompleted.Get();
		params.type = PdfRendererThreadParams::Open;
		params.result = false;

		auto CreatePdfRendererWorkerThreadIfNeeded = [&params]()
		{
			if (!s_thread.IsValid())
			{
				Win78Libraries::load();
				if (Win78Libraries::RoGetActivationFactory == nullptr)
					return false;
				s_thread.Attach(CreateThread(nullptr, 0, PdfRendererWorkerThread, &params, 0, &s_dwThreadId));
				if (!s_thread.IsValid())
					return false;
			}
			else
			{
				if (!PostThreadMessage(s_dwThreadId, WM_USER, 0, reinterpret_cast<LPARAM>(&params)))
					return false;
			}
			return true;
		};
		EnterCriticalSection(&Win78Libraries::CriticalSection);
		bool result = CreatePdfRendererWorkerThreadIfNeeded();
		if (result)
			WaitForSingleObject(params.hEvent, INFINITE);
		LeaveCriticalSection(&Win78Libraries::CriticalSection);
		if (!result)
			return false;

		m_pageCount = params.pageCount;
		m_docId = params.docId;

		return params.result;
	}

	virtual void render(Image& img, int page, float zoom) override
	{
		if (!isValid())
			return;

		Microsoft::WRL::Wrappers::Event renderCompleted(CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, WRITE_OWNER | EVENT_ALL_ACCESS));

		wchar_t szTempPath[MAX_PATH];
		GetTempPath(static_cast<DWORD>(std::size(szTempPath)), szTempPath);
		wchar_t szFileName[MAX_PATH];
		GetTempFileName(szTempPath, L"pdf", 0, szFileName);

		PdfRendererThreadParams params{};
		params.filename = szFileName;
		params.hEvent = renderCompleted.Get();
		params.page = page;
		params.zoom = zoom;
		params.type = PdfRendererThreadParams::Render;
		params.docId = m_docId;
			
		if (!PostThreadMessage(s_dwThreadId, WM_USER, 0, reinterpret_cast<LPARAM>(&params)))
			return;

		WaitForSingleObject(params.hEvent, INFINITE);

		img.load(szFileName);

		DeleteFile(szFileName);
	}

	virtual unsigned getPageCount() const override
	{
		return m_pageCount;
	}

private:

	struct PdfRendererThreadParams
	{
		enum Type { Open, Render, Close };
		const wchar_t *filename;
		Type type;
		int page;
		float zoom;
		unsigned pageCount;
		HANDLE hEvent;
		bool result;
		int docId;
	};

	static bool LoadPdf(const wchar_t *filename, ABI::Windows::Data::Pdf::IPdfDocument **ppPdfDocument)
	{
		// https://dev.activebasic.com/egtra/2015/12/24/853/

		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IRandomAccessStream> s;
		HRESULT hr = CreateRandomAccessStreamOnFile(filename, static_cast<DWORD>(ABI::Windows::Storage::FileAccessMode_Read),
			IID_PPV_ARGS(&s));
		if (FAILED(hr))
			return false;

		Microsoft::WRL::ComPtr<ABI::Windows::Data::Pdf::IPdfDocumentStatics> pPdfDocumentsStatics;
		hr = Win78Libraries::RoGetActivationFactory(
			Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Data_Pdf_PdfDocument).Get(),
			IID_PPV_ARGS(&pPdfDocumentsStatics));
		if (FAILED(hr))
			return false;

		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Data::Pdf::PdfDocument*>> pAsync;
		hr = pPdfDocumentsStatics->LoadFromStreamAsync(s.Get(), &pAsync);
		if (FAILED(hr))
			return false;

		return SUCCEEDED(Win78Libraries::await(pAsync.Get(), ppPdfDocument));
	}

	static bool RenderPdfPage(ABI::Windows::Data::Pdf::IPdfDocument *pPdfDocument, int page, float zoom, const wchar_t *filename)
	{
		if (pPdfDocument == nullptr)
			return false;

		Microsoft::WRL::ComPtr<ABI::Windows::Data::Pdf::IPdfPage> pPdfPage;
		auto hr = pPdfDocument->GetPage(page, &pPdfPage);
		if (FAILED(hr))
			return false;

		Microsoft::WRL::Wrappers::FileHandle file(CreateFile(filename, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file.IsValid())
			return false;
		file.Close();

		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IRandomAccessStream> s;
		hr = CreateRandomAccessStreamOnFile(filename, static_cast<DWORD>(ABI::Windows::Storage::FileAccessMode_ReadWrite),
			IID_PPV_ARGS(&s));
		if (FAILED(hr))
			return false;

		Microsoft::WRL::ComPtr<ABI::Windows::Data::Pdf::IPdfPageRenderOptions> pPdfPageRenderOptions;
		hr = Win78Libraries::RoActivateInstance(
			Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions).Get(),
			&pPdfPageRenderOptions);
		if (FAILED(hr))
			return false;

		ABI::Windows::Foundation::Size pageSize;
		pPdfPage->get_Size(&pageSize);
		pPdfPageRenderOptions->put_DestinationWidth(static_cast<unsigned>(pageSize.Width * zoom));
		pPdfPageRenderOptions->put_DestinationHeight(static_cast<unsigned>(pageSize.Height * zoom));

		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncAction> pAsyncAction;
		hr = pPdfPage->RenderWithOptionsToStreamAsync(s.Get(), pPdfPageRenderOptions.Get(), &pAsyncAction);
		if (FAILED(hr))
			return false;

		return SUCCEEDED(Win78Libraries::await(pAsyncAction.Get()));
	}

	static DWORD WINAPI PdfRendererWorkerThread(LPVOID lpParam)
	{
		if (SUCCEEDED(CoInitializeEx(0, COINIT_MULTITHREADED)))
		{
			int docId = 0;
			Microsoft::WRL::ComPtr<ABI::Windows::Data::Pdf::IPdfDocument> pPdfDocumentLast;
			std::map<int, Microsoft::WRL::ComPtr<ABI::Windows::Data::Pdf::IPdfDocument>> mapPdfDocument;
			PdfRendererThreadParams* pParam = reinterpret_cast<PdfRendererThreadParams*>(lpParam);
			for (;;)
			{
				if (pParam->type == PdfRendererThreadParams::Open)
				{
					mapPdfDocument.insert_or_assign(docId, nullptr);
					pParam->docId = docId++;
					pParam->result = LoadPdf(pParam->filename, &mapPdfDocument.at(pParam->docId));
					if (mapPdfDocument.at(pParam->docId))
						mapPdfDocument.at(pParam->docId)->get_PageCount(&pParam->pageCount);
				}
				else if (pParam->type == PdfRendererThreadParams::Render)
				{
					if (mapPdfDocument.find(pParam->docId) != mapPdfDocument.end())
						pParam->result = RenderPdfPage(mapPdfDocument.at(pParam->docId).Get(), pParam->page, pParam->zoom, pParam->filename);
				}
				else
				{
					if (mapPdfDocument.find(pParam->docId) != mapPdfDocument.end())
					{
						if (mapPdfDocument.size() == 1)
							pPdfDocumentLast = mapPdfDocument.at(pParam->docId); // Workaround to avoid crashes on process exit
						mapPdfDocument.erase(pParam->docId);
						pParam->result = true;
					}
					else
					{
						pParam->result = false;
					}
				}
				SetEvent(pParam->hEvent);

				MSG msg;
				BOOL bRet = GetMessage(&msg, nullptr, 0, 0);
				if (bRet == 0 || bRet == -1)
					break;
				pParam = reinterpret_cast<PdfRendererThreadParams*>(msg.lParam);
			}
			CoUninitialize();
		}
		return true;
	}

private:
	using Thread = Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits>;
	inline static Thread s_thread;
	inline static DWORD s_dwThreadId;
	float m_imageWidth = 0.0f;
	float m_imageHeight = 0.0f;
	unsigned m_pageCount = 0;
	int m_docId = 0;
};

class SvgRenderer: public ImageRenderer
{
public:
	virtual bool isValid() const override
	{
		return m_pSvgDocument != nullptr;
	}

	virtual bool load(const wchar_t *filename) override
	{
		Win78Libraries::load();
		if (Win78Libraries::D2D1CreateFactory == nullptr)
			return false;

		if (!m_pD2DFactory)
		{
			if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory, nullptr, &m_pD2DFactory)))
				return false;
		}

		if (!m_pDCRenderTarget)
		{
			const auto props = D2D1::RenderTargetProperties(
				D2D1_RENDER_TARGET_TYPE_DEFAULT,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
				0,
				0,
				D2D1_RENDER_TARGET_USAGE_NONE,
				D2D1_FEATURE_LEVEL_DEFAULT
			);
			if (FAILED(m_pD2DFactory->CreateDCRenderTarget(&props, &m_pDCRenderTarget)))
				return false;
		}

		Microsoft::WRL::ComPtr<IStream> pStream;
		if (FAILED(::SHCreateStreamOnFileEx(filename, STGM_READ, 0, FALSE, nullptr, &pStream)))
			return false;

		Microsoft::WRL::ComPtr<ID2D1DeviceContext5> pDeviceContext5;
		if (FAILED(m_pDCRenderTarget->QueryInterface(IID_PPV_ARGS(&pDeviceContext5))))
			return false;
		if (FAILED(pDeviceContext5->CreateSvgDocument(pStream.Get(), { 1, 1 }, &m_pSvgDocument)))
			return false;

		calcSize();
		return true;
	}

	virtual void render(Image& img, int page, float zoom) override
	{
		if (!isValid())
			return;
		RECT rc{ 0, 0, static_cast<long>(m_imageWidth * zoom), static_cast<long>(m_imageHeight * zoom) };
		HDC hDC = GetDC(nullptr);
		HDC hMemDC = CreateCompatibleDC(hDC);
		HBITMAP hBitmap = CreateCompatibleBitmap(hDC, rc.right - rc.left, rc.bottom - rc.top);
		HBITMAP hOldBitmap = static_cast<HBITMAP>(SelectObject(hMemDC, hBitmap));

		m_pDCRenderTarget->BindDC(hMemDC, &rc);
		m_pDCRenderTarget->BeginDraw();
		Microsoft::WRL::ComPtr<ID2D1DeviceContext5> pDeviceContext5;
		m_pDCRenderTarget->QueryInterface(IID_PPV_ARGS(&pDeviceContext5));
		m_pSvgDocument->SetViewportSize({ m_imageWidth, m_imageHeight });
		pDeviceContext5->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, { 0, 0 }));
		pDeviceContext5->DrawSvgDocument(m_pSvgDocument.Get());
		m_pDCRenderTarget->EndDraw();

		img.getFipImage()->copyFromBitmap(hBitmap);

		SelectObject(hMemDC, hOldBitmap);
		DeleteObject(hBitmap);
		DeleteDC(hMemDC);
		ReleaseDC(nullptr, hDC);
	}

	virtual unsigned getPageCount() const override { return 1; }

private:
	void calcSize()
	{
		Microsoft::WRL::ComPtr<ID2D1SvgElement> pSvgElement;
		D2D1_SVG_VIEWBOX viewBox{};
		float width = 0.0f;
		float height = 0.0f;

		m_pSvgDocument->GetRoot(&pSvgElement);
		pSvgElement->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, static_cast<void*>(&viewBox), sizeof(viewBox));
		pSvgElement->GetAttributeValue(L"width", &width);
		pSvgElement->GetAttributeValue(L"height", &height);

		if (viewBox.width != 0.0f && viewBox.height != 0.0f)
		{
			m_imageWidth = viewBox.width;
			m_imageHeight = viewBox.height;
		}
		if (height != 0.0f && width != 0.0f)
		{
			m_imageWidth = width;
			m_imageHeight = height;
		}
	}

private:
	Microsoft::WRL::ComPtr<ID2D1Factory> m_pD2DFactory;
	Microsoft::WRL::ComPtr<ID2D1DCRenderTarget>  m_pDCRenderTarget;
	Microsoft::WRL::ComPtr<ID2D1SvgDocument> m_pSvgDocument;
	float m_imageWidth = 0.0f;
	float m_imageHeight = 0.0f;
};

class WICRenderer: public ImageRenderer
{
public:
	virtual bool isValid() const override
	{
		return m_pBitmapDecoder != nullptr;
	}

	virtual bool load(const wchar_t *filename) override
	{
		m_pBitmapDecoder.Reset();

		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pFactory)); 
		// Create a decoder from the file.
		if (FAILED(hr))
			return false;

		hr = m_pFactory->CreateDecoderFromFilename(filename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &m_pBitmapDecoder);
		if (FAILED(hr))
			return false;

		return true;
	}

	virtual void render(Image& img, int page, float zoom) override
	{
		Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> pBitmapFrameDecode;
		if (FAILED(m_pBitmapDecoder->GetFrame(page, &pBitmapFrameDecode)))
			return;

		Microsoft::WRL::ComPtr<IWICFormatConverter> pFormatConverter;
		if (FAILED(m_pFactory->CreateFormatConverter(&pFormatConverter)))
			return;

		if (FAILED(pFormatConverter->Initialize(pBitmapFrameDecode.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			return;

		unsigned imageWidth = 0, imageHeight = 0;
		if (FAILED(pFormatConverter->GetSize(&imageWidth, &imageHeight)))
			return;

		std::vector<BYTE> buffer(imageWidth * imageHeight * 4);
		WICRect rc = { 0, 0, static_cast<int>(imageWidth), static_cast<int>(imageHeight) };
		if (FAILED(pFormatConverter->CopyPixels(&rc, imageWidth * 4, static_cast<unsigned>(buffer.size()), buffer.data())))
			return;

		fipImageEx* pImage = img.getFipImage();
		pImage->setSize(FIT_BITMAP, imageWidth, imageHeight, 32);
		for (unsigned y = 0; y < imageHeight; ++y)
			memcpy(pImage->getScanLine(y), buffer.data() + (imageHeight - 1 - y) * imageWidth * 4, imageWidth * 4);
	}

	virtual unsigned getPageCount() const override
	{
		if (!m_pBitmapDecoder)
			return 1;
		UINT count = 0;
		if (FAILED(m_pBitmapDecoder->GetFrameCount(&count)))
			return 1;
		return count;
	}

private:
	Microsoft::WRL::ComPtr<IWICImagingFactory> m_pFactory;
	Microsoft::WRL::ComPtr<IWICBitmapDecoder> m_pBitmapDecoder;
};

#endif

class GdiPlusRenderer: public ImageRenderer
{
public:
	virtual bool isValid() const override
	{
		return m_pMetafile != nullptr;
	}

	virtual bool load(const wchar_t *filename) override
	{
		m_pMetafile.reset(new Gdiplus::Metafile(filename));
		if (!m_pMetafile)
			return false;
		
		std::wstring ext = std::filesystem::path(filename).extension().generic_wstring();
		if (_wcsicmp(ext.c_str(), L".wmf") == 0)
		{
			std::unique_ptr<Gdiplus::Bitmap> pBitmap(Gdiplus::Bitmap::FromFile(filename));
			m_imageWidth = pBitmap->GetWidth();
			m_imageHeight = pBitmap->GetHeight();
		}
		else
		{
			m_imageWidth = m_pMetafile->GetWidth();
			m_imageHeight = m_pMetafile->GetHeight();
		}
		return true;
	}

	virtual void render(Image& img, int page, float zoom) override
	{
		HBITMAP hBitmap = nullptr;
		Gdiplus::Bitmap bitmap(static_cast<unsigned>(m_imageWidth * zoom), static_cast<unsigned>(m_imageHeight * zoom));
		std::unique_ptr<Gdiplus::Graphics> pGraphics(Gdiplus::Graphics::FromImage(&bitmap));
		pGraphics->ScaleTransform(zoom, zoom);
		pGraphics->DrawImage(m_pMetafile.get(), 0, 0);
		bitmap.GetHBITMAP({ 0, 0, 0, 0 }, &hBitmap);
		img.getFipImage()->copyFromBitmap(hBitmap);
		DeleteObject(hBitmap);
	}

	virtual unsigned getPageCount() const override { return 1; }

private:
	std::unique_ptr<Gdiplus::Metafile> m_pMetafile;
	unsigned m_imageWidth = 0;
	unsigned m_imageHeight = 0;
};

class ImgConverter
{
public:
	enum class ImageType
	{
		NotSupported,
		PDF,
		SVG,
		EMF,
		WMF,
		WICSupported,
	};

	static ImageType getImageType(const wchar_t *filename)
	{
		std::wstring ext = std::filesystem::path(filename).extension().generic_wstring();
		if (_wcsicmp(ext.c_str(), L".emf") == 0)
			return ImageType::EMF;
		else if (_wcsicmp(ext.c_str(), L".wmf") == 0)
			return ImageType::WMF;
#ifdef _WIN64
		else if (_wcsicmp(ext.c_str(), L".pdf") == 0)
			return ImageType::PDF;
		else if (_wcsicmp(ext.c_str(), L".svg") == 0)
			return ImageType::SVG;
		return ImageType::WICSupported;
#else
		return ImageType::NotSupported;
#endif
	}

	static bool isSupportedImage(const wchar_t *filename)
	{
		return getImageType(filename) != ImageType::NotSupported;
	}

	bool isValid() const
	{
		return (m_pRenderer != nullptr && m_pRenderer->isValid());
	}

	bool load(const wchar_t *filename)
	{
		switch (getImageType(filename))
		{
#ifdef _WIN64
		case ImageType::PDF:
			m_pRenderer.reset(new PdfRenderer());
			break;
		case ImageType::SVG:
			m_pRenderer.reset(new SvgRenderer());
			break;
		case ImageType::WICSupported:
			m_pRenderer.reset(new WICRenderer());
			break;
#endif
		case ImageType::EMF:
		case ImageType::WMF:
			m_pRenderer.reset(new GdiPlusRenderer());
			break;
		default:
			return false;
		}
		return m_pRenderer->load(filename);
	}

	void close()
	{
		m_pRenderer.reset();
	}

	void render(Image& img, int page, float zoom)
	{
		m_pRenderer->render(img, page, zoom);
	}

	unsigned getPageCount() const
	{
		return m_pRenderer->getPageCount();
	}

private:
	std::unique_ptr<ImageRenderer> m_pRenderer;
};

