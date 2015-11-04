#ifndef _IMAGE_INPUT_H
#define _IMAGE_INPUT_H 1

#include <memory>
#include <string>

#include <movit/flat_input.h>


// An output that takes its input from a static image, loaded with ffmpeg.
// comes from a single 2D array with chunky pixels.
class ImageInput : public movit::FlatInput {
public:
	ImageInput(const std::string &filename);

	std::string effect_type_id() const override { return "ImageInput"; }

private:
	std::unique_ptr<uint8_t[]> image_data;
};

#endif // !defined(_IMAGE_INPUT_H)
