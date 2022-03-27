/*
 * Audio Overload SDK
 *
 * Wave dumping
 *
 * Author: Nmlgc
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ao.h"
#include "utils.h"
#include "wavedump.h"

typedef struct {
	uint32 FOURCC;
	uint32 Size;
} RIFFCHUNK;

// This is pretty much identical to the WAVEFORMAT+PCMWAVEFORMAT structures
// from <windows.h>, which we can't use because they're stupidly declared in
// such a way as to come with 2 padding bytes before and after
// PCMWAVEFORMAT.wBitsPerSample.

typedef struct {
	uint16 wFormatTag; // format type
	uint16 nChannels; // number of channels (i.e. mono, stereo, etc.)
	uint32 nSamplesPerSec; // sample rate
	uint32 nAvgBytesPerSec; // for buffer estimation
	uint16 nBlockAlign; // block size of data
	uint16 wBitsPerSample;
} WAVEFORMAT_PCM;

// flags for wFormatTag field of WAVEFORMAT
#define WAVE_FORMAT_PCM 1

typedef struct {
	RIFFCHUNK cRIFF;
	uint32 WAVE;
	RIFFCHUNK cfmt;
	WAVEFORMAT_PCM Format;
	RIFFCHUNK cdata;
} WAVEHEADER;

typedef struct {
	RIFFCHUNK ccue;
	uint32 points;	// number of cue points
} CUEHEADER;

typedef struct {
	uint32 dwName; // unique identification value
	uint32 dwPosition; // play order position
	uint32 fccChunk; // RIFF ID of corresponding data chunk
	uint32 dwChunkStart; // offset from [fccChunk] to the LIST chunk, or 0 if no such chunk is present
	uint32 dwBlockStart; // offset from [fccChunk] to a (unspecified) "block" containing the sample
	uint32 dwSampleOffset; // offset from [dwBlockStart] to the sample
} CUEPOINT;

#define BUFFSIZE 32768

static void wavedump_mem(wavedump_t *wave, const void *src_buf, size_t src_len)
{
	uint32 i;
	i = wave->len + src_len;
	if (i > wave->memsize) {
		wave->memsize = wave->memsize * 1.5;
		wave->mem = realloc(wave->mem, wave->memsize);
		if (!wave->mem) {
			fprintf(stderr, "ERROR: Could not allocate output buffer.\n");
			exit(-1);
		}
	}
	memcpy(wave->mem + wave->len, src_buf, src_len);
	wave->len = i;
}

static void wavedump_write(wavedump_t *wave, const void *src_buf, size_t src_len)
{
	if (wave->ismem){
		wavedump_mem(wave, src_buf, src_len);
	}
	else {
		fwrite(src_buf, src_len, 1, wave->file);
	}
}

static void wavedump_LIST_adtl_labl_write(
	wavedump_t *wave, uint32 point_id, const char *label
)
{
	RIFFCHUNK cLIST;
	const uint32 adtl = LE32(*(uint32*)"adtl");
	RIFFCHUNK clabl;
	size_t label_len;

	assert(wave);
	assert(label);

	label_len = strlen(label) + 1;
	clabl.FOURCC = LE32(*(uint32*)"labl");
	cLIST.FOURCC = LE32(*(uint32*)"LIST");
	clabl.Size = sizeof(point_id) + label_len;
	cLIST.Size = sizeof(adtl) + sizeof(clabl) + clabl.Size;

	wavedump_write(wave, &cLIST, sizeof(cLIST));
	wavedump_write(wave, &adtl, sizeof(adtl));
	wavedump_write(wave, &clabl, sizeof(clabl));
	wavedump_write(wave, &point_id, sizeof(point_id));
	wavedump_write(wave, label, label_len);
}

static void wavedump_header_fill(
	WAVEHEADER *h, uint32 data_size, uint32 file_size,
	uint32 sample_rate, uint16 bits_per_sample, uint16 channels
)
{
	h->cRIFF.FOURCC = LE32(*(uint32*)"RIFF");
	h->cRIFF.Size = LE32(file_size - sizeof(RIFFCHUNK));
	h->WAVE = LE32(*(uint32*)"WAVE");
	h->cfmt.FOURCC = LE32(*(uint32*)"fmt ");
	h->cfmt.Size = LE32(sizeof(h->Format));
	h->Format.wFormatTag = LE16(WAVE_FORMAT_PCM);
	h->Format.nChannels = LE16(channels);
	h->Format.nSamplesPerSec = LE16(sample_rate);
	h->Format.nBlockAlign = LE16((channels * bits_per_sample) / 8);
	h->Format.nAvgBytesPerSec = LE32(
		h->Format.nSamplesPerSec * h->Format.nBlockAlign
	);
	h->Format.wBitsPerSample = LE16(bits_per_sample);
	h->cdata.FOURCC = LE32(*(uint32*)"data");
	h->cdata.Size = LE32(data_size);
}

ao_bool wavedump_open(wavedump_t *wave, const char *fn)
{
	uint32 temp = 0;
	assert(wave);
	WAVEHEADER h;
	wave->data_size = 0;
	wave->loop_sample = 0;
	wave->len = 0;
	wave->ismem = false;

	if (!strcmp(fn, "-")) {
		wave->file = stdout;
		wave->memsize = (8*1024*1024);
		wave->mem = malloc(wave->memsize);
		if (!wave->mem) {
			fprintf(stderr, "Could not allocate output buffer.\n");
			return false;
		}
		wave->ismem = true;
	}
	else {
		wave->file = fopen_derivative(fn, ".wav");
	}

	if(!wave->file) {
		return false;
	}

	if (wave->ismem) {
		// Jump over header
		memset(wave->mem, 0, sizeof(h));
		wave->len = sizeof(h);
	}
	else {
		// Jump over header, we write that one later
		fwrite(&temp, 1, sizeof(h), wave->file);
	}
	return true;
}

void wavedump_loop_set(wavedump_t *wave, uint32 loop_sample)
{
	assert(wave);
	assert(wave->file);
	wave->loop_sample = loop_sample;
}

void wavedump_append(wavedump_t *wave, uint32 len, void *buf)
{
	assert(wave);
	if(wave->file) {
		wave->data_size += len;
		// XXX: Doesn't the data need to be swapped on big-endian platforms?
		// That would mean that we need to know the target wave format on
		// opening time.
		wavedump_write(wave, buf, len);
	}
}

void wavedump_finish(
	wavedump_t *wave, uint32 sample_rate, uint16 bits_per_sample, uint16 channels
)
{
	assert(wave);
	if(wave->file) {
		WAVEHEADER h;
		uint32 file_size;
		// RIFF chunks have to be word-aligned, so we have to pad out the
		// data chunk if the number of samples happens to be odd.
		if(wave->data_size & 1) {
			uint8 pad = 0;
			// fwrite() rather than wavedump_append(), as the chunk size
			// obviously doesn't include the padding.
			wavedump_write(wave, &pad, sizeof(pad));
		}
		if(wave->loop_sample) {
			// Write the "cue " chunk, as well as an additional
			// LIST-adtl-labl chunk for newer GoldWave versions
			CUEHEADER cue;
			CUEPOINT point;

			cue.ccue.FOURCC = LE32(*(uint32*)"cue ");
			cue.ccue.Size = LE32(4 + 1 * sizeof(CUEPOINT));
			cue.points = LE32(1);
			point.dwName = LE32(0);
			point.dwChunkStart = LE32(0);
			point.dwBlockStart = LE32(0);
			point.dwPosition = LE32(wave->loop_sample);
			point.dwSampleOffset = LE32(wave->loop_sample);
			point.fccChunk = LE32(*(uint32*)"data");

			wavedump_write(wave, &cue, sizeof(cue));
			wavedump_write(wave, &point, sizeof(point));
			wavedump_LIST_adtl_labl_write(wave, 0, "Loop point");
		}

		if (wave->ismem) {
			file_size = wave->len;
		}
		else {
			file_size = ftell(wave->file);
		}
		wavedump_header_fill(
			&h, wave->data_size, file_size,
			sample_rate, bits_per_sample, channels
		);

		if (wave->ismem) {
			// Insert the proper wav header
			memcpy(wave->mem, &h, sizeof(h));

			uint32 bytes, i, q, r, bq;

			bytes = BUFFSIZE * sizeof(stereo_sample_t) * 2;
			q = file_size / bytes;
			r = file_size % bytes;
			bq = bytes * q;

			// fprintf(stderr, "WAV length: %d\n", file_size);
			// fprintf(stderr, "bytes: %d\n", bytes);
			// fprintf(stderr, "quotient: %d\n", q);
			// fprintf(stderr, "remainder: %d\n", r);
			// fprintf(stderr, "bytes*quotient: %d\n", bq);

			setvbuf(stdout, NULL, _IOFBF, bytes);

			for (i = 0; i < bq; ) {
				if (fwrite(wave->mem + i, 1, bytes, stdout) != bytes) {
					fprintf(stderr, "ERROR: Failed writing output.\n");
					break;
					}
				i += bytes;
				if (i == bq) {
					// fprintf(stderr, "i: %d\n", i);
					if (fwrite(wave->mem + bq, 1, r, stdout) != r) {
					fprintf(stderr, "ERROR: Failed writing output.\n");
					break;
					}
				}
			}
			free(wave->mem);
		}
		else {
			fseek(wave->file, 0, SEEK_SET);
			fwrite(&h, sizeof(h), 1, wave->file);
		}
		fclose(wave->file);
		wave->file = NULL;
	}
}
