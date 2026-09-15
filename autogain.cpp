#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <cmath>



// config
static constexpr uint32_t RATE = 48000;
static constexpr uint32_t CHANNELS = 2;
static constexpr uint32_t STRIDE = sizeof(float) * CHANNELS;

static constexpr uint32_t RING_FRAMES = 32768;
static constexpr float TARGET = 1.0f;   // data max, cible
static constexpr float MAX_GAIN = 80.0f;

static constexpr float RELEASE_DELAY = 30.0f;   // remontée en secondes
//static constexpr float RELEASE_DELAY = 0.5f;   // remontée en secondes
static constexpr float HOLD = 10.0f; // attente avant remontée
//static constexpr float HOLD = 0.01f; // attente avant remontée


// fin config

static constexpr float RELEASE_RATE = 20.0f / ( RATE * RELEASE_DELAY );   // remontée en secondes

// memoire
struct Data {
	pw_main_loop *loop;
	pw_stream *input;
	pw_stream *output;
	float gain = 1.0f;
	float hold = 0.0f;

	spa_ringbuffer ring;
	float ring_data[RING_FRAMES * CHANNELS];
};



static void on_input(void *userdata)
{
	auto *d = static_cast<Data *>(userdata);

	pw_buffer *b = pw_stream_dequeue_buffer(d->input);
	if (!b)
		return;

	spa_buffer *buf = b->buffer;
	auto *chunk = buf->datas[0].chunk;
	auto *src = static_cast<uint8_t *>(buf->datas[0].data);

	if (!src || !chunk) {
		pw_stream_queue_buffer(d->input, b);
		return;
	}

	uint32_t frames = chunk->size / STRIDE;
	uint32_t index = 0;

	int32_t filled = spa_ringbuffer_get_write_index(
		&d->ring, &index);

	if (filled >= 0) {
		uint32_t space = RING_FRAMES - filled;
		uint32_t count = std::min(frames, space);

		if (count > 0) {
			spa_ringbuffer_write_data(
				&d->ring,
				d->ring_data,
				sizeof(d->ring_data),
				(index % RING_FRAMES) * STRIDE,
				src + chunk->offset,
				count * STRIDE);

			spa_ringbuffer_write_update(
				&d->ring, index + count);
		}
	}

	pw_stream_queue_buffer(d->input, b);
}



static void on_output(void *userdata)
{
	auto *d = static_cast<Data *>(userdata); // map

	// import
	pw_buffer *b = pw_stream_dequeue_buffer(d->output);   if( !b ){ return; }

	spa_buffer *buf = b->buffer;
	auto *dst = static_cast<uint8_t *>(buf->datas[0].data);   if( !dst ){ pw_stream_queue_buffer(d->output, b); return; }

	// size
	uint32_t frames = buf->datas[0].maxsize / STRIDE;
	if( b->requested ){ frames = std::min( frames, static_cast<uint32_t>(b->requested)); }

	uint32_t index = 0;
	int32_t available = spa_ringbuffer_get_read_index(&d->ring, &index);
	uint32_t count = 0;

	if( available > 0 ){ count = std::min( static_cast<uint32_t>(available), frames); }

	// rewrite import
	if( count > 0 ){
		spa_ringbuffer_read_data( &d->ring, d->ring_data, sizeof(d->ring_data), (index % RING_FRAMES) * STRIDE, dst, count * STRIDE);
		spa_ringbuffer_read_update( &d->ring, index + count);
	}

	// Silence si le buffer manque de données
	if( count < frames ){ std::memset( dst + count * STRIDE, 0, (frames - count) * STRIDE); }

	// si positif alors calculer gain
	float part_peak = 0.0f;
	float part_gain = MAX_GAIN;
	float *samples = reinterpret_cast<float *>(dst);

	for( uint32_t i = 0; i < frames * CHANNELS; i+=20 ){ part_peak = std::max(part_peak, std::fabs(samples[i])); }

	// verifier signal
	if( part_peak > 0.0f ){ part_gain = std::min(MAX_GAIN, TARGET / part_peak); }

	if( part_gain < d->gain ){

		// Peak trop fort : baisse immédiate
		d->gain = part_gain;
		d->hold = HOLD;
	}else if( d->hold > 0.0f ){

		// baisse recente, wait
		d->hold -= (float)frames / RATE;
	}else{

		// montée douce
		d->gain += frames * RELEASE_RATE;
		if( d->gain > part_gain ){ d->gain = part_gain; }
	}

	// Applique UN seul gain aux deux canaux
	for( uint32_t i = 0; i < frames * CHANNELS; ++i ){ samples[i] *= d->gain; }

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = STRIDE;
	buf->datas[0].chunk->size = frames * STRIDE;

	pw_stream_queue_buffer(d->output, b);
}



//apis
static const pw_stream_events input_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_input
};

static const pw_stream_events output_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_output
};



// init et montage
int main()
{
	pw_init(nullptr, nullptr);

	Data d{};
	spa_ringbuffer_init(&d.ring);

	d.loop = pw_main_loop_new(nullptr);

	// ----- Entrée : Audio/Sink -----

	d.input = pw_stream_new_simple(
		pw_main_loop_get_loop(d.loop),
		"Mini Auto Gain",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "DSP",
			PW_KEY_MEDIA_CLASS, "Audio/Sink",
			PW_KEY_NODE_NAME, "Mini Auto Gain",
			PW_KEY_NODE_DESCRIPTION, "Mini Auto Gain",
			PW_KEY_NODE_LINK_GROUP, "mini-autogain",
			"filter.smart", "true",
			"filter.smart.name", "Mini Auto Gain",
			nullptr),
		&input_events,
		&d);

	// ----- Sortie : Stream/Output/Audio -----

	d.output = pw_stream_new_simple(
		pw_main_loop_get_loop(d.loop),
		"Mini Auto Gain Output",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "DSP",
			PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
			PW_KEY_NODE_NAME, "Mini Auto Gain Output",
			PW_KEY_NODE_LINK_GROUP, "mini-autogain",
			PW_KEY_NODE_PASSIVE, "true",
			"stream.dont-remix", "true",
			nullptr),
		&output_events,
		&d);

	// ----- Format -----

	spa_audio_info_raw audio{};
	audio.format = SPA_AUDIO_FORMAT_F32;
	audio.channels = CHANNELS;
	audio.rate = RATE;

	uint8_t in_buffer[1024];
	uint8_t out_buffer[1024];

	spa_pod_builder in_builder =
		SPA_POD_BUILDER_INIT(in_buffer, sizeof(in_buffer));

	spa_pod_builder out_builder =
		SPA_POD_BUILDER_INIT(out_buffer, sizeof(out_buffer));

	const spa_pod *in_params =
		spa_format_audio_raw_build(
			&in_builder,
			SPA_PARAM_EnumFormat,
			&audio);

	const spa_pod *out_params =
		spa_format_audio_raw_build(
			&out_builder,
			SPA_PARAM_EnumFormat,
			&audio);

	int r1 = pw_stream_connect(
		d.input,
		PW_DIRECTION_INPUT,
		PW_ID_ANY,
		static_cast<pw_stream_flags>(
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS),
		&in_params,
		1);

	int r2 = pw_stream_connect(
		d.output,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		static_cast<pw_stream_flags>(
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS),
		&out_params,
		1);

	if (r1 < 0 || r2 < 0) {
		std::cerr << "Erreur PipeWire : "
				  << r1 << " / " << r2 << "\n";
		return 1;
	}

	std::cout << "Mini Auto Gain actif - gain x1\n";

	pw_main_loop_run(d.loop);

	pw_stream_destroy(d.output);
	pw_stream_destroy(d.input);
	pw_main_loop_destroy(d.loop);

	pw_deinit();
	return 0;
}
