#include "pch.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Numerics;
    using namespace Windows::UI;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Imaging;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Streams;
}

namespace util
{
    using namespace robmikh::common::uwp;
    using namespace robmikh::common::desktop;
}

struct Options
{
    bool UseDebugLayer;
    std::wstring FramesPath;
    std::wstring BackgroundPath;
    std::wstring OutputPath;
};

enum class CliResult
{
    Valid,
    Invalid,
    Help,
};

CliResult ParseOptions(std::vector<std::wstring> const& args, Options& options);
void PrintHelp();
std::future<std::vector<winrt::com_ptr<ID2D1Bitmap1>>> LoadBitmapsAsync(
    winrt::com_ptr<ID3D11Device> d3dDevice, 
    winrt::com_ptr<ID2D1DeviceContext> d2dContext, 
    std::wstring path);
winrt::com_ptr<ID2D1Bitmap1> CreateBitmapFromTexture(
    winrt::com_ptr<ID3D11Texture2D> const& texture,
    winrt::com_ptr<ID2D1DeviceContext> const& d2dContext);
winrt::IAsyncOperation<winrt::BitmapEncoder> CreateGifEncoderAsync(winrt::IRandomAccessStream stream);

winrt::IAsyncAction MainAsync(bool useDebugLayer, std::wstring framesPath, std::wstring backgroundPath, std::wstring outputPath)
{
    // Initialize D3D
    uint32_t flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (useDebugLayer)
    {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    auto d3dDevice = util::CreateD3DDevice();
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    d3dDevice->GetImmediateContext(d3dContext.put());

    // Initialize D2D
    auto debugLevel = D2D1_DEBUG_LEVEL_NONE;
    if (useDebugLayer)
    {
        debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
    }
    auto d2dFactory = util::CreateD2DFactory(debugLevel);
    auto d2dDevice = util::CreateD2DDevice(d2dFactory, d3dDevice);
    winrt::com_ptr<ID2D1DeviceContext> d2dContext;
    winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext.put()));

    // Load all frames
    auto frames = co_await LoadBitmapsAsync(d3dDevice, d2dContext, framesPath);
    if (frames.empty())
    {
        wprintf(L"No frames found, exiting...\n");
        co_return;
    }
    // Make sure all the frames are the same size
    auto frameSize = frames[0]->GetPixelSize();
    for (auto&& frame : frames)
    {
        auto size = frame->GetPixelSize();
        if (size.width != frameSize.width || size.height != frameSize.height)
        {
            throw winrt::hresult_invalid_argument(L"All frames must be of the same size!");
        }
    }

    // Load the backgrounds
    auto backgrounds = co_await LoadBitmapsAsync(d3dDevice, d2dContext, backgroundPath);
    for (auto&& background : backgrounds)
    {
        auto size = background->GetPixelSize();
        if (size.width != frameSize.width || size.height != frameSize.height)
        {
            throw winrt::hresult_invalid_argument(L"All backgrounds must be of the same size as the frames!");
        }
    }
    
    // Create our output file
    auto outputFile = co_await util::CreateStorageFileFromPathAsync(outputPath);

    // Create our render target
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = frameSize.width;
    desc.Height = frameSize.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.SampleDesc.Count = 1;
    winrt::com_ptr<ID3D11Texture2D> renderTargetTexture;
    winrt::check_hresult(d3dDevice->CreateTexture2D(&desc, nullptr, renderTargetTexture.put()));
    auto renderTarget = CreateBitmapFromTexture(renderTargetTexture, d2dContext);

    // Create our background template
    winrt::com_ptr<ID3D11Texture2D> backgroundTemplateTexture;
    winrt::check_hresult(d3dDevice->CreateTexture2D(&desc, nullptr, backgroundTemplateTexture.put()));
    auto backgroundTemplate = CreateBitmapFromTexture(backgroundTemplateTexture, d2dContext);

    // Create our staging texture
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    winrt::check_hresult(d3dDevice->CreateTexture2D(&desc, nullptr, stagingTexture.put()));

    // Draw our background template
    d2dContext->SetTarget(backgroundTemplate.get());
    auto clearColor = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 1.0f };
    d2dContext->BeginDraw();
    d2dContext->Clear(&clearColor);
    for (auto&& background : backgrounds)
    {
        d2dContext->DrawBitmap(background.get());
    }
    winrt::check_hresult(d2dContext->EndDraw());

    // Iterate through each frame and compose it with the background template. After that,
    // extract the image and encode it as a frame.
    uint32_t frameDelay = 13;
    d2dContext->SetTarget(renderTarget.get());
    {
        auto stream = co_await outputFile.OpenAsync(winrt::FileAccessMode::ReadWrite);
        auto encoder = co_await CreateGifEncoderAsync(stream);

        for (auto i = 0; i < frames.size(); i++)
        {
            auto&& frame = frames[i];

            // Render the frame
            d2dContext->BeginDraw();
            d2dContext->DrawBitmap(backgroundTemplate.get());
            d2dContext->DrawBitmap(frame.get());
            winrt::check_hresult(d2dContext->EndDraw());
            d3dContext->CopyResource(stagingTexture.get(), renderTargetTexture.get());

            // Get the bytes out of the render target
            auto bytes = util::CopyBytesFromTexture(stagingTexture);

            // Write our frame delay
            co_await encoder.BitmapProperties().SetPropertiesAsync(
                {
                    { L"/grctlext/Delay", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt16(static_cast<uint16_t>(frameDelay)), winrt::PropertyType::UInt16) },
                });

            encoder.SetPixelData(
                winrt::BitmapPixelFormat::Bgra8,
                winrt::BitmapAlphaMode::Premultiplied,
                frameSize.width,
                frameSize.height,
                1.0,
                1.0,
                bytes);

            if (i < frames.size() - 1)
            {
                co_await encoder.GoToNextFrameAsync();
            }
        }

        co_await encoder.FlushAsync();
    }
    
    wprintf(L"Done!\n");

    co_return;
}

int __stdcall wmain(int argc, wchar_t* argv[])
{
    // Initialize COM
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // CLI
    std::vector<std::wstring> args(argv + 1, argv + argc);
    Options options = {};
    auto cliResult = ParseOptions(args, options);
    switch (cliResult)
    {
    case CliResult::Help:
        return 0;
    case CliResult::Invalid:
        return 1;
    default:
        break;
    }

    MainAsync(options.UseDebugLayer, options.FramesPath, options.BackgroundPath, options.OutputPath).get();

    return 0;
}

CliResult ParseOptions(std::vector<std::wstring> const& args, Options& options)
{
    using namespace robmikh::common::wcli::impl;

    if (GetFlag(args, L"-help") || GetFlag(args, L"/?"))
    {
        PrintHelp();
        return CliResult::Help;
    }
    auto framesPath = GetFlagValue(args, L"-f", L"/f");
    if (framesPath.empty())
    {
        wprintf(L"Invalid frames path! Use '-help' for help.\n");
        return CliResult::Invalid;
    }
    auto backgroundPath = GetFlagValue(args, L"-b", L"/b");
    if (backgroundPath.empty())
    {
        wprintf(L"Invalid background path! Use '-help' for help.\n");
        return CliResult::Invalid;
    }
    auto outputPath = GetFlagValue(args, L"-o", L"/o");
    if (outputPath.empty())
    {
        wprintf(L"Invalid output path! Use '-help' for help.\n");
        return CliResult::Invalid;
    }
    auto useDebugLayer = GetFlag(args, L"-dxDebug", L"/dxDebug");

    options.UseDebugLayer = useDebugLayer;
    options.FramesPath = framesPath;
    options.BackgroundPath = backgroundPath;
    options.OutputPath = outputPath;
    return CliResult::Valid;
}

void PrintHelp()
{
    wprintf(L"GifCompose.exe\n");
    wprintf(L"An application that creates gifs from files.\n");
    wprintf(L"\n");
    wprintf(L"Arguments:\n");
    wprintf(L"  -f <frames path>         (required) Path to the frame images.\n");
    wprintf(L"  -b <backgrounds path>    (required) Path to the background images.\n");
    wprintf(L"  -o <output path>         (required) Path to the output image that will be created.\n");
    wprintf(L"\n");
    wprintf(L"Flags:\n");
    wprintf(L"  -dxDebug           (optional) Use the DirectX and DirectML debug layers.\n");
    wprintf(L"\n");
}

std::future<std::vector<winrt::com_ptr<ID2D1Bitmap1>>> LoadBitmapsAsync(
    winrt::com_ptr<ID3D11Device> d3dDevice,
    winrt::com_ptr<ID2D1DeviceContext> d2dContext,
    std::wstring path)
{
    // Find all the files in the folder
    auto fullPath = std::filesystem::canonical(path);
    if (!std::filesystem::is_directory(fullPath))
    {
        throw winrt::hresult_invalid_argument(L"Path was not a folder!");
    }
    std::vector<std::wstring> files;
    for (auto const& entry : std::filesystem::directory_iterator(fullPath))
    {
        auto path = entry.path();
        if (std::filesystem::is_regular_file(path) && path.has_extension())
        {
            auto extension = path.extension().wstring();
            if (extension == L".png")
            {
                files.push_back(path.filename().wstring());
            }
        }
    }
    // Sort the filesnames
    std::sort(files.begin(), files.end());

    // Get the storage folder for the folder
    auto folder = co_await winrt::StorageFolder::GetFolderFromPathAsync(fullPath.wstring());
    std::vector<winrt::com_ptr<ID2D1Bitmap1>> bitmaps;
    for (auto&& filename : files)
    {
        auto file = co_await folder.GetFileAsync(filename);
        auto stream = co_await file.OpenReadAsync();

        auto texture = co_await util::LoadTextureFromStreamAsync(stream, d3dDevice);
        auto bitmap = CreateBitmapFromTexture(texture, d2dContext);
        bitmaps.push_back(bitmap);
    }

    co_return bitmaps;
}

winrt::com_ptr<ID2D1Bitmap1> CreateBitmapFromTexture(
    winrt::com_ptr<ID3D11Texture2D> const& texture,
    winrt::com_ptr<ID2D1DeviceContext> const& d2dContext)
{
    auto dxgiSurface = texture.as<IDXGISurface>();
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    winrt::check_hresult(d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.get(), nullptr, bitmap.put()));
    return bitmap;
}

winrt::IAsyncOperation<winrt::BitmapEncoder> CreateGifEncoderAsync(winrt::IRandomAccessStream stream)
{
    // Setup our encoder
    auto encoder = co_await winrt::BitmapEncoder::CreateAsync(winrt::BitmapEncoder::GifEncoderId(), stream);
    auto containerProperties = encoder.BitmapContainerProperties();
    // Write the application block
    // http://www.vurdalakov.net/misc/gif/netscape-looping-application-extension
    std::string text("NETSCAPE2.0");
    std::vector<uint8_t> chars(text.begin(), text.end());
    WINRT_VERIFY(chars.size() == 11);
    co_await containerProperties.SetPropertiesAsync(
        {
            { L"/appext/application", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt8Array(chars), winrt::PropertyType::UInt8Array) },
            // The first value is the size of the block, which is the fixed value 3.
            // The second value is the looping extension, which is the fixed value 1.
            // The third and fourth values comprise an unsigned 2-byte integer (little endian).
            //     The value of 0 means to loop infinitely.
            // The final value is the block terminator, which is the fixed value 0.
            { L"/appext/data", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt8Array({ 3, 1, 0, 0, 0 }), winrt::PropertyType::UInt8Array) },
        });
    co_return encoder;
}