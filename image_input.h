#ifndef _IMAGE_INPUT_H
#define _IMAGE_INPUT_H 1

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <time.h>

#include <movit/flat_input.h>

// An output that takes its input from a static image, loaded with ffmpeg.
// comes from a single 2D array with chunky pixels. The image is refreshed
// from disk about every second.
class ImageInput : public movit::FlatInput {
public:
	ImageInput(const std::string &filename);

	std::string effect_type_id() const override { return "ImageInput"; }
	void set_gl_state(GLuint glsl_program_num, const std::string& prefix, unsigned *sampler_num) override;

private:
	struct Image {
		std::unique_ptr<uint8_t[]> pixels;
		timespec last_modified;
	};

	std::string filename;
	std::shared_ptr<const Image> current_image;

	static std::shared_ptr<const Image> load_image(const std::string &filename);
	static std::shared_ptr<const Image> load_image_raw(const std::string &filename);
	static void update_thread_func(const std::string &filename, const timespec &first_modified);
	static std::mutex all_images_lock;
	static std::map<std::string, std::shared_ptr<const Image>> all_images;
	static std::map<std::string, std::thread> update_threads;
};

#endif // !defined(_IMAGE_INPUT_H)
