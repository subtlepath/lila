// Images are not decoded in previews: the parser sees every format as unsupported and falls back to alt text.
#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <FontAlloc.h>
#include <GfxRenderer.h>

#include <cstdlib>

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t, int64_t, ImageDimensions&, const char*) {
  return false;
}

void ImageBlock::render(GfxRenderer&, int, int) {}
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int) const {}
bool ImageBlock::needsDecode() const { return false; }
bool ImageBlock::serialize(HalFile&) { return false; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }

extern "C" {
void* fiFontMalloc(size_t size) { return std::malloc(size); }
void* fiFontRealloc(void* ptr, size_t size) { return std::realloc(ptr, size); }
void fiFontFree(void* ptr) { std::free(ptr); }
}
