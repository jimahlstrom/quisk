/*
 * This modue provides sound access for QUISK using the ALSA
 * library for Linux.
*/
#ifdef QUISK_HAVE_ALSA
#include <Python.h>
#include <complex.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include "quisk.h"

/*
 The sample rate is in frames per second.  Each frame has a number of channels,
 and each channel has a sample of size sample_bytes.  The channels are interleaved:
 (channel0, channel1), (channel0, channel1), ...
*/

extern struct sound_conf quisk_sound_state;	// Current sound status

static int is_little_endian;			// Test byte order; is it little-endian?
static double mic_playbuf_util = 0.70;		// Current mic play buffer utilization 0.0 to 1.0
static union {
	short buffer2[SAMP_BUFFER_SIZE];			// Buffer for 2-byte samples from sound
	unsigned char buffer3[3 * SAMP_BUFFER_SIZE];		// Buffer for 3-byte samples from sound
	int buffer4[SAMP_BUFFER_SIZE];				// Buffer for 4-byte samples from sound
} bufs ;
static int bufferz[SAMP_BUFFER_SIZE];				// Buffer for zero samples

static snd_pcm_sframes_t frames_in_buffer(struct sound_dev * dev)
{  // return the number of frames in the play buffer
	snd_pcm_sframes_t avail_to_write;

	if ((avail_to_write = snd_pcm_avail(dev->handle)) < 0) {
		dev->dev_error++;
		if (quisk_sound_state.verbose_sound)
			printf("frames_in_buffer: Failure for pcm_avail\n");
		return -1;
	}
	return dev->play_buf_size - avail_to_write;
}

static snd_pcm_sframes_t write_frames(struct sound_dev * dev, void * buffer, int count)
{
	snd_pcm_sframes_t frames;

	if (count <= 0)
		return 0;
	frames = snd_pcm_writei (dev->handle, buffer, count);
	if (frames <= 0) {
		if (frames == -EPIPE) {	// underrun
			quisk_sound_state.underrun_error++;
			dev->dev_underrun++;
			if (quisk_sound_state.verbose_sound)
				printf("Underrun %s\n", dev->stream_description);
		}
		else {
			quisk_sound_state.write_error++;
			dev->dev_error++;
			if (quisk_sound_state.verbose_sound)
				printf("Error write_frames %s\n", dev->stream_description);
		}
		snd_pcm_prepare(dev->handle);
		frames = snd_pcm_writei (dev->handle, buffer, count);
	}
	return frames;
}

int quisk_read_alsa(struct sound_dev * dev, complex double * cSamples)
{	// cSamples can be NULL to discard samples.
	// Read sound samples from the ALSA soundcard.
	// Samples are converted to 32 bits with a range of +/- CLIP32 and placed into cSamples.
	int i;
	snd_pcm_sframes_t frames, delay, avail;
	short si, sq;
	int ii, qq;
	int nSamples;

	if (!dev->handle)
		return -1;

	switch(snd_pcm_state(dev->handle)) {
	case SND_PCM_STATE_RUNNING:
		break;
	case SND_PCM_STATE_PREPARED:
		break;
	case SND_PCM_STATE_XRUN:
#if DEBUG_IO
		QuiskPrintTime("read_alsa: Capture overrun", 0);
#endif
		snd_pcm_prepare(dev->handle);
		break;
	default:
#if DEBUG_IO
		QuiskPrintTime("read_alsa: State UNKNOWN", 0);
#endif
		break;
	}

	if (snd_pcm_avail_delay(dev->handle, &avail, &delay) >= 0) {
		dev->dev_latency = avail + delay;	// avail frames can be read plus delay frames digitized but can't be read yet
	}
	else {
		avail = 32;
		dev->dev_latency = 0;
		dev->dev_error++;
#if DEBUG_IO
		QuiskPrintTime("read_alsa: snd_pcm_avail_delay failed", 0);
#endif
	}
	if (dev->read_frames == 0) {		// non-blocking: read available frames
		if (avail < 32)
			avail = 32;	// read frames to restart from error
	}
	else {
		avail = dev->read_frames;	// size of read request
	}
	i = SAMP_BUFFER_SIZE * 8 / 10 / dev->num_channels;	// limit read request to buffer size
	if (avail > i)
		avail = i;
	nSamples = 0;
	switch (dev->sample_bytes) {
	case 2:
		frames = snd_pcm_readi (dev->handle, bufs.buffer2, avail);	// read samples
		if ( ! cSamples)
			return 0;
		if (frames == -EAGAIN) {	// no samples available
			break;
		}
		else if (frames <= 0) {		// error
			dev->dev_error++;
#if DEBUG_IO
			QuiskPrintTime("read_alsa: frames < 0", 0);
#endif
			snd_pcm_prepare (dev->handle);
			snd_pcm_start (dev->handle);
			break;
		}
		for (i = 0; frames; i += dev->num_channels, frames--) {
			si = bufs.buffer2[i + dev->channel_I];
			sq = bufs.buffer2[i + dev->channel_Q];
			if (si >=  CLIP16 || si <= -CLIP16)
				dev->overrange++;	// assume overrange returns max int
			if (sq >=  CLIP16 || sq <= -CLIP16)
				dev->overrange++;
			ii = si << 16;
			qq = sq << 16;
			cSamples[nSamples] = ii + I * qq;
			nSamples++;
		}
		break;
	case 3:
		frames = snd_pcm_readi (dev->handle, bufs.buffer3, avail);	// read samples
		if ( ! cSamples)
			return 0;
		if (frames == -EAGAIN) {	// no samples available
			break;
		}
		else if (frames <= 0) {		// error
			dev->dev_error++;
#if DEBUG_IO
			QuiskPrintTime("read_alsa: frames < 0", 0);
#endif
			snd_pcm_prepare (dev->handle);
			snd_pcm_start (dev->handle);
			break;
		}
		for (i = 0; frames; i += dev->num_channels, frames--) {
			ii = qq = 0;
			if (!is_little_endian) {	// convert to big-endian
				*((unsigned char *)&ii    ) = bufs.buffer3[(i + dev->channel_I) * 3 + 2];
				*((unsigned char *)&ii + 1) = bufs.buffer3[(i + dev->channel_I) * 3 + 1];
				*((unsigned char *)&ii + 2) = bufs.buffer3[(i + dev->channel_I) * 3    ];
				*((unsigned char *)&qq    ) = bufs.buffer3[(i + dev->channel_Q) * 3 + 2];
				*((unsigned char *)&qq + 1) = bufs.buffer3[(i + dev->channel_Q) * 3 + 1];
				*((unsigned char *)&qq + 2) = bufs.buffer3[(i + dev->channel_Q) * 3    ];
			}
			else {		// convert to little-endian
				memcpy((unsigned char *)&ii + 1, bufs.buffer3 + (i + dev->channel_I) * 3, 3);
				memcpy((unsigned char *)&qq + 1, bufs.buffer3 + (i + dev->channel_Q) * 3, 3);
			}
			if (ii >=  CLIP32 || ii <= -CLIP32)
				dev->overrange++;	// assume overrange returns max int
			if (qq >=  CLIP32 || qq <= -CLIP32)
				dev->overrange++;
			cSamples[nSamples] = ii + I * qq;
			nSamples++;
		}
		break;
	case 4:
		frames = snd_pcm_readi (dev->handle, bufs.buffer4, avail);	// read samples
		if ( ! cSamples)
			return 0;
		if (frames == -EAGAIN) {	// no samples available
			break;
		}
		else if (frames <= 0) {		// error
			dev->dev_error++;
#if DEBUG_IO
			QuiskPrintTime("read_alsa: frames < 0", 0);
#endif
			snd_pcm_prepare (dev->handle);
			snd_pcm_start (dev->handle);
			break;
		}
		for (i = 0; frames; i += dev->num_channels, frames--) {
			ii = bufs.buffer4[i + dev->channel_I];
			qq = bufs.buffer4[i + dev->channel_Q];
			if (ii >=  CLIP32 || ii <= -CLIP32)
				dev->overrange++;	// assume overrange returns max int
			if (qq >=  CLIP32 || qq <= -CLIP32)
				dev->overrange++;
			cSamples[nSamples] = ii + I * qq;
			nSamples++;
		}
		break;
	default:
		return 0;
	}
	if ( ! strcmp(dev->stream_description, "Microphone Input")) {
		if (mic_playbuf_util > 0.85) {		// Remove a sample
			nSamples--;
#if DEBUG_IO
			printf("read_alsa %s: Remove a mic sample, util %.2lf\n", dev->stream_description, mic_playbuf_util);
#endif
		}
		else if(cSamples && mic_playbuf_util < 0.55 && nSamples >= 2) {	// Add a sample
			cSamples[nSamples] = cSamples[nSamples - 1];
			cSamples[nSamples - 1] = (cSamples[nSamples - 2] + cSamples[nSamples]) / 2.0;
			nSamples++;
#if DEBUG_IO
			printf("read_alsa %s: Add a mic sample, util %.2lf\n", dev->stream_description, mic_playbuf_util);
#endif
		}
	}
	return nSamples;
}

void quisk_alsa_sidetone(struct sound_dev * dev)
{
	int i, bytes_per_sample, bytes_per_frame, ch_I, ch_Q, new_key;
	snd_pcm_sframes_t frames, nFrames, rewindable;
	snd_pcm_uframes_t buffer_size, period_size;
	void * ptSample;
	unsigned char * buffer;

	if ( ! dev->handle)
		return;
	if (snd_pcm_state(dev->handle) == SND_PCM_STATE_XRUN) {
		if (quisk_sound_state.verbose_sound)
			printf("alsa_sidetone: underrun\n");
		quisk_sound_state.underrun_error++;
		dev->dev_underrun++;
		snd_pcm_prepare(dev->handle);
	}
	if (snd_pcm_get_params (dev->handle, &buffer_size, &period_size) != 0) {
		dev->dev_error++;
		if (quisk_sound_state.verbose_sound)
			printf("alsa_sidetone: Failure for get_params\n");
		return;
	}
	nFrames = dev->latency_frames - frames_in_buffer(dev);	// write desired latency less fill level frames
	new_key = QUISK_CWKEY_DOWN;
	if (new_key != dev->old_key) {	// key changed, empty buffer and refill
		dev->old_key = new_key;
		rewindable = snd_pcm_rewindable(dev->handle);
		rewindable -= period_size;
		if (rewindable > 0) {
			snd_pcm_rewind(dev->handle, rewindable);
			nFrames = dev->latency_frames - period_size;
			quisk_make_sidetone(dev, rewindable);
		}
	}
	if (nFrames <= 0)
		return;
	bytes_per_sample = dev->sample_bytes;
	bytes_per_frame = dev->sample_bytes * dev->num_channels;
	buffer = (unsigned char *)bufs.buffer4;
	ch_I = dev->channel_I;
	ch_Q = dev->channel_Q;
	for (i = 0; i < nFrames; i++) {
		ptSample = quisk_make_sidetone(dev, 0);
		memcpy(buffer + ch_I * bytes_per_sample, ptSample, bytes_per_sample);
		memcpy(buffer + ch_Q * bytes_per_sample, ptSample, bytes_per_sample);
		buffer += bytes_per_frame;
	}
	frames = write_frames(dev, bufs.buffer4, nFrames);
	if (quisk_sound_state.verbose_sound && (frames != nFrames))
		printf("alsa_sidetone: %s bad write %ld %ld\n", dev->stream_description, nFrames, frames);
}

void quisk_play_alsa(struct sound_dev * playdev, int nSamples,
		complex double * cSamples, int report_latency, double volume)
{	// Play the samples; write them to the ALSA soundcard.
	int i, n, index, buffer_frames;
	snd_pcm_sframes_t frames, rewind;
	int ii, qq;

#if DEBUG_IO
	static int timer=0;
#endif

	if (!playdev->handle || nSamples <= 0)
		return;
	if (snd_pcm_state(playdev->handle) == SND_PCM_STATE_XRUN) {
		if (quisk_sound_state.verbose_sound)
			printf("play_alsa: underrun on %s\n", playdev->stream_description);
		quisk_sound_state.underrun_error++;
		playdev->dev_underrun++;
		snd_pcm_prepare(playdev->handle);
	}
	buffer_frames = frames_in_buffer(playdev);
	playdev->dev_latency = buffer_frames;
	if (report_latency) {		// Report for main playback device
		quisk_sound_state.latencyPlay = buffer_frames;		// samples in play buffer
	}
	playdev->cr_average_fill += (double)(buffer_frames + nSamples / 2) / playdev->play_buf_size;
	playdev->cr_average_count++;
	if (playdev->dev_index == t_MicPlayback)
		mic_playbuf_util = (double)(nSamples + buffer_frames) / playdev->play_buf_size;
#if DEBUG_IO
	timer += nSamples;
	if (timer > playdev->sample_rate) {
		timer = 0;
		printf("play_alsa %s: Samples new %d old %d total %d latency_frames %d\n",
			playdev->stream_description, nSamples, buffer_frames, nSamples + buffer_frames, playdev->latency_frames);
	}
#endif

	
	if (nSamples + buffer_frames > playdev->play_buf_size) {	// rewind some frames to go back to the fill level latency_frames
		rewind = nSamples + buffer_frames - playdev->latency_frames;
		if (rewind > buffer_frames)
			rewind = buffer_frames;
		snd_pcm_rewind(playdev->handle, rewind);
		quisk_sound_state.write_error++;
		playdev->dev_error++;
		if (quisk_sound_state.verbose_sound)
			printf("play_alsa: Buffer overflow in %s\n", playdev->stream_description);
	}
	index = 0;
	switch(playdev->sound_format) {
	case Int16:
		while (index < nSamples) {
			for (i = 0, n = index; n < nSamples; i += playdev->num_channels, n++) {
				ii = (int)(volume * creal(cSamples[n]) / 65536);
				qq = (int)(volume * cimag(cSamples[n]) / 65536);
				bufs.buffer2[i + playdev->channel_I] = (short)ii;
				bufs.buffer2[i + playdev->channel_Q] = (short)qq;
			}
			n = n - index;
			frames = write_frames(playdev, bufs.buffer2, n);
			if (frames <= 0)
				index = nSamples;	// give up
			else
				index += frames;
		}
		break;
	case Int24:
		while (index < nSamples) {
			for (i = 0, n = index; n < nSamples; i += playdev->num_channels, n++) {
				ii = (int)(volume * creal(cSamples[n]) / 256);
				qq = (int)(volume * cimag(cSamples[n]) / 256);
				if (!is_little_endian) {	// convert to big-endian
					bufs.buffer3[(i + playdev->channel_I) * 3    ] = *((unsigned char *)&ii + 2);
					bufs.buffer3[(i + playdev->channel_Q) * 3    ] = *((unsigned char *)&qq + 2);
					bufs.buffer3[(i + playdev->channel_I) * 3 + 1] = *((unsigned char *)&ii + 1);
					bufs.buffer3[(i + playdev->channel_Q) * 3 + 1] = *((unsigned char *)&qq + 1);
					bufs.buffer3[(i + playdev->channel_I) * 3 + 2] = *((unsigned char *)&ii    );
					bufs.buffer3[(i + playdev->channel_Q) * 3 + 2] = *((unsigned char *)&qq    );
				}
				else {	// convert to little-endian
					memcpy(bufs.buffer3 + (i + playdev->channel_I) * 3, (unsigned char *)&ii, 3);
					memcpy(bufs.buffer3 + (i + playdev->channel_Q) * 3, (unsigned char *)&qq, 3);
				}
			}
			n = n - index;
			frames = write_frames(playdev, bufs.buffer3, n);
			if (frames <= 0)
				index = nSamples;	// give up
			else
				index += frames;
		}
		break;
	case Int32:
		while (index < nSamples) {
			for (i = 0, n = index; n < nSamples; i += playdev->num_channels, n++) {
				ii = (int)(volume * creal(cSamples[n]));
				qq = (int)(volume * cimag(cSamples[n]));
				bufs.buffer4[i + playdev->channel_I] = ii;
				bufs.buffer4[i + playdev->channel_Q] = qq;
			}
			n = n - index;
			frames = write_frames(playdev, bufs.buffer4, n);
			if (frames <= 0)
				index = nSamples;	// give up
			else
				index += frames;
		}
		break;
	case Float32:
		break;
	}
}

static int device_list(PyObject * py, snd_pcm_stream_t stream, char * name)
{	// return 1 if the card name was substituted
	snd_ctl_t *handle;
	int card, err, dev;
	char buf100[100];
	const char * card_text, * pcm_text;
	snd_ctl_card_info_t *info;
	snd_pcm_info_t *pcminfo;

	snd_ctl_card_info_alloca(&info);
	snd_pcm_info_alloca(&pcminfo);

	card = -1;
	if (snd_card_next(&card) < 0 || card < 0) {
		printf("no soundcards found...\n");
		return 0;
	}
	while (card >= 0) {
		sprintf(buf100, "hw:%d", card);
		if ((err = snd_ctl_open(&handle, buf100, 0)) < 0) {
			printf("device_list: control open (%i): %s", card, snd_strerror(err));
			goto next_card;
		}
		if ((err = snd_ctl_card_info(handle, info)) < 0) {
			printf("device_list: control hardware info (%i): %s", card, snd_strerror(err));
			snd_ctl_close(handle);
			goto next_card;
		}
		dev = -1;
		while (1) {
			if (snd_ctl_pcm_next_device(handle, &dev)<0)
				printf("device_list: snd_ctl_pcm_next_device\n");
			if (dev < 0)
				break;
			snd_pcm_info_set_device(pcminfo, dev);
			snd_pcm_info_set_subdevice(pcminfo, 0);
			snd_pcm_info_set_stream(pcminfo, stream);
			card_text = snd_ctl_card_info_get_name(info);
			if ( ! card_text || ! card_text[0])
				card_text = snd_ctl_card_info_get_id(info);
			if ((err = snd_ctl_pcm_info(handle, pcminfo)) < 0) {
				if (err != -ENOENT)
					printf ("device_list: control digital audio info (%i): %s", card, snd_strerror(err));
				continue;
			}
			else {
				pcm_text = snd_pcm_info_get_name(pcminfo);
				if ( ! pcm_text || ! pcm_text[0])
					pcm_text = snd_pcm_info_get_id(pcminfo);
			}
			snprintf(buf100, 100, "%s %s (hw:%d,%d)", card_text, pcm_text, card, dev);
			if (py) {		// add to list of devices
				PyList_Append(py, PyString_FromString(buf100));
			}
			if (name) {		// return the "hw:" name
				if (strstr(buf100, name)) {
					snprintf(name, QUISK_SC_SIZE, "hw:%d,%d", card, dev);
					snd_ctl_close(handle);
					return 1;
				}
			}
		}
		snd_ctl_close(handle);
	next_card:
		if (snd_card_next(&card) < 0) {
			printf("snd_card_next\n");
			break;
		}
	}
	return 0;
}

PyObject * quisk_alsa_sound_devices(PyObject * self, PyObject * args)
{	// Return a list of ALSA device names [pycapt, pyplay]
	PyObject * pylist, * pycapt, * pyplay;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	// Each pycapt and pyplay is [pydev, pyname]
	pylist = PyList_New(0);		// list [pycapt, pyplay]
	pycapt = PyList_New(0);		// list of capture devices
	pyplay = PyList_New(0);		// list of play devices
	PyList_Append(pylist, pycapt);
	PyList_Append(pylist, pyplay);
	device_list(pycapt, SND_PCM_STREAM_CAPTURE, NULL);
	device_list(pyplay, SND_PCM_STREAM_PLAYBACK, NULL);
	return pylist;
}

static snd_pcm_format_t check_formats(struct sound_dev * dev, snd_pcm_hw_params_t *hware)
{
	snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
	dev->sample_bytes = 0;
#if 0
	char * card_name;
	int card;
	snd_pcm_info_t * pcm_info;
	printf("driver %s\n", snd_ctl_card_info_get_driver(info));
	int loopback = 0;
	if (snd_pcm_info_malloc(&pcm_info) == 0) {
		if (snd_pcm_info (dev->handle, pcm_info) == 0) {
			card = snd_pcm_info_get_card (pcm_info);
			if (card >= 0) {
				if (snd_card_get_name(card, &card_name) == 0) {
					if (strcmp(card_name, "Loopback") == 0)
						loopback = 1;
					printf("name %s\n", card_name);
					free(card_name);
				}
			}
		}
		snd_pcm_info_free(pcm_info);
	}
#endif
	strMcpy (dev->msg1, "Available formats: ", QUISK_SC_SIZE);
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_S16) == 0) {
		if (!dev->sample_bytes) {
			strncat(dev->msg1, "*", QUISK_SC_SIZE);
			dev->sample_bytes = 2;
			dev->sound_format = Int16;
			format = SND_PCM_FORMAT_S16;
		}
		strncat(dev->msg1, "S16 ", QUISK_SC_SIZE);
	}
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_S32) == 0) {
		if (!dev->sample_bytes) {
			strncat(dev->msg1, "*", QUISK_SC_SIZE);
			dev->sample_bytes = 4;
			dev->sound_format = Int32;
			format = SND_PCM_FORMAT_S32;
		}
		strncat(dev->msg1, "S32 ", QUISK_SC_SIZE);
	}
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_U32) == 0) {
		strncat(dev->msg1, "U32 ", QUISK_SC_SIZE);
	}
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_S24) == 0) {
		strncat(dev->msg1, "S24 ", QUISK_SC_SIZE);
	}
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_U24) == 0) {
		strncat(dev->msg1, "U24 ", QUISK_SC_SIZE);
	}
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_S24_3LE) == 0) {
		if (!dev->sample_bytes) {
			strncat(dev->msg1, "*", QUISK_SC_SIZE);
			dev->sample_bytes = 3;
			dev->sound_format = Int24;
			format = SND_PCM_FORMAT_S24_3LE;
		}
		strncat(dev->msg1, "S24_3LE ", QUISK_SC_SIZE);
	}
	if (snd_pcm_hw_params_test_format (dev->handle, hware, SND_PCM_FORMAT_U16) == 0) {
		strncat(dev->msg1, "U16 ", QUISK_SC_SIZE);
	}
	if (format == SND_PCM_FORMAT_UNKNOWN)
		strncat(dev->msg1, "*UNSUPPORTED", QUISK_SC_SIZE);
	else
		snd_pcm_hw_params_set_format (dev->handle, hware, format);
	return format;
}

static int quisk_open_alsa_capture(struct sound_dev * dev)
{	// Open the ALSA soundcard for capture.  Return non-zero for error.
	int i, err, dir, sample_rate, mode;
	int poll_size;
	unsigned int ui;
	char buf[QUISK_SC_SIZE];
	snd_pcm_hw_params_t *hware;
	snd_pcm_sw_params_t *sware;
	snd_pcm_uframes_t frames;
	snd_pcm_t * handle;

	if ( ! dev->name[0])	// Check for null capture name
		return 0;

	if (quisk_sound_state.verbose_sound)
		printf("*** Capture %s on alsa name %s device %s\n", dev->stream_description, dev->name, dev->device_name);
	if (dev->read_frames == 0)
		mode = SND_PCM_NONBLOCK;
	else
		mode = 0;
	if ( ! strncmp (dev->name, "alsa:", 5)) {	// search for the name in info strings, put device name into buf
		strMcpy(buf, dev->name + 5, QUISK_SC_SIZE);
		device_list(NULL, SND_PCM_STREAM_CAPTURE, buf);
	}
	else {		// just try to open the device
		strMcpy(buf, dev->device_name, QUISK_SC_SIZE);
	}
	for (i = 0; i < 6; i++) {	// try a few times in case the device is busy
		if (quisk_sound_state.verbose_sound)
			printf("    Try %d to open %s\n", i, buf);
		err = snd_pcm_open (&handle, buf, SND_PCM_STREAM_CAPTURE, mode);
		if (err >= 0)
			break;
		QuiskSleepMicrosec(500000);
	}
	if (err < 0) {
		snprintf(quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot open capture device %.40s (%.40s)",
				dev->name, snd_strerror (err));
		strMcpy(dev->dev_errmsg, quisk_sound_state.err_msg, QUISK_SC_SIZE);
		if (quisk_sound_state.verbose_sound)
			printf("%s\n", quisk_sound_state.err_msg);
		return 1;
	}
	dev->handle = handle;
	dev->driver = DEV_DRIVER_ALSA;
	dev->old_key = 0;
	if ((err = snd_pcm_sw_params_malloc (&sware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot allocate software parameter structure (%s)\n",
				snd_strerror (err));
		if (quisk_sound_state.verbose_sound)
			printf("%s\n", quisk_sound_state.err_msg);
		return 1;
	}
	if ((err = snd_pcm_hw_params_malloc (&hware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot allocate hardware parameter structure (%s)\n",
				snd_strerror (err));
		if (quisk_sound_state.verbose_sound)
			printf("%s\n", quisk_sound_state.err_msg);
		snd_pcm_sw_params_free (sware);
		return 1;
	}
	if ((err = snd_pcm_hw_params_any (handle, hware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot initialize capture parameters (%s)\n",
				snd_strerror (err));
		goto errend;
	}
	/* UNAVAILABLE
	if ((err = snd_pcm_hw_params_set_rate_resample (handle, hware, 0)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot disable resampling (%s)\n",
			snd_strerror (err));
		goto errend;
	}
	*/
	// Get some parameters to send back
	if (snd_pcm_hw_params_get_rate_min(hware, &dev->rate_min, &dir) != 0)
		dev->rate_min = 0;	// Error
	if (snd_pcm_hw_params_get_rate_max(hware, &dev->rate_max, &dir) != 0)
		dev->rate_max = 0;	// Error
	if (snd_pcm_hw_params_get_channels_min(hware, &dev->chan_min) != 0)
		dev->chan_min= 0;	// Error
	if (snd_pcm_hw_params_get_channels_max(hware, &dev->chan_max) != 0)
		dev->chan_max= 0;	// Error
	if (quisk_sound_state.verbose_sound) {
		printf("    Sample rate min %d  max %d\n",  dev->rate_min, dev->rate_max);
		printf("    Sample rate requested %d\n", dev->sample_rate);
		printf("    Number of channels min %d  max %d\n",  dev->chan_min, dev->chan_max);
		printf("    Capture channels are %d %d\n", dev->channel_I, dev->channel_Q);
	}
	// Set the capture parameters
	if (check_formats(dev, hware) == SND_PCM_FORMAT_UNKNOWN) {
		strMcpy(quisk_sound_state.msg1, dev->msg1, QUISK_SC_SIZE);
		strMcpy (quisk_sound_state.err_msg, "Quisk does not support your capture format.", QUISK_SC_SIZE);
		goto errend;
	}
	strMcpy(quisk_sound_state.msg1, dev->msg1, QUISK_SC_SIZE);
	sample_rate = dev->sample_rate;
	if (snd_pcm_hw_params_set_rate (handle, hware, sample_rate, 0) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Can not set sample rate %d",
			sample_rate);
		goto errend;
	}
	if (snd_pcm_hw_params_set_access (handle, hware, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
		strMcpy(quisk_sound_state.err_msg, "Interleaved access is not available", QUISK_SC_SIZE);
		goto errend;
	}
	if (snd_pcm_hw_params_get_channels_min(hware, &ui) != 0)
		ui = 0;	// Error
	if (dev->num_channels < (int)ui)		// increase number of channels to minimum available
		dev->num_channels = ui;
	if (snd_pcm_hw_params_set_channels (handle, hware, dev->num_channels) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Can not set channels to %d", dev->num_channels);
		goto errend;
	}
	// Try to set a capture buffer larger than needed
	frames = sample_rate * 200 / 1000;	// buffer size in milliseconds
	if (snd_pcm_hw_params_set_buffer_size_near (handle, hware, &frames) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Can not set capture buffer size");
		goto errend;
	}
	dev->play_buf_size = frames;	// play_buf_size used for capture buffer size too
	poll_size = (int)(quisk_sound_state.data_poll_usec * 1e-6 * sample_rate + 0.5);
	if ((int)frames < poll_size * 3) {		// buffer size is too small, reduce poll time
		quisk_sound_state.data_poll_usec = (int)(frames * 1.e6 / sample_rate / 3 + 0.5);
#if DEBUG_IO
		printf("Reduced data_poll_usec %d for small sound capture buffer\n",
			quisk_sound_state.data_poll_usec);
#endif
	}
	if (quisk_sound_state.verbose_sound) {
		printf("    %s\n", dev->msg1);
		printf("    Capture buffer size %d\n", dev->play_buf_size);
		if ((int)frames > SAMP_BUFFER_SIZE / dev->num_channels)
			printf("Capture buffer exceeds size of sample buffers\n");
	}
	if ((err = snd_pcm_hw_params (handle, hware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set hw capture parameters (%s)\n",
				snd_strerror (err));
		goto errend;
	}
	if ((err = snd_pcm_sw_params_current (handle, sware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot get software capture parameters (%s)\n",
				snd_strerror (err));
		goto errend;
	}

	if ((err = snd_pcm_prepare (handle)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot prepare capture interface for use (%s)\n",
			snd_strerror (err));
		goto errend;
	}
	// Success
	snd_pcm_hw_params_free (hware);
	snd_pcm_sw_params_free (sware);
	if (quisk_sound_state.verbose_sound)
		printf("*** End capture on alsa device %s %s\n", dev->name, quisk_sound_state.err_msg);
	return 0;
errend:
	snd_pcm_hw_params_free (hware);
	snd_pcm_sw_params_free (sware);
	if (quisk_sound_state.verbose_sound) {
		printf("*** Error end for capture on alsa device %s %s\n", dev->name, quisk_sound_state.err_msg);
	}
	return 1;
}

static int quisk_open_alsa_playback(struct sound_dev * dev)
{	// Open the ALSA soundcard for playback.  Return non-zero on error.
	int i, err, dir, mode;
	unsigned int ui;
	char buf[QUISK_SC_SIZE];
	snd_pcm_hw_params_t *hware;
	snd_pcm_sw_params_t *sware;
	snd_pcm_uframes_t frames, buffer_size, period_size;
	snd_pcm_t * handle;

	if ( ! dev->name[0])	// Check for null play name
		return 0;

	if (quisk_sound_state.verbose_sound)
		printf("*** Playback %s, alsa name %s, device %s\n", dev->stream_description, dev->name, dev->device_name);
	if (dev->read_frames == 0)
		mode = SND_PCM_NONBLOCK;
	else
		mode = 0;
	if ( ! strncmp (dev->name, "alsa:", 5)) {	// search for the name in info strings, put device name into buf
		strMcpy(buf, dev->name + 5, QUISK_SC_SIZE);
		device_list(NULL, SND_PCM_STREAM_PLAYBACK, buf);
	}
	else {		// just try to open the device
		strMcpy(buf, dev->device_name, QUISK_SC_SIZE);
	}
	for (i = 0; i < 6; i++) {	// try a few times in case the device is busy
		if (quisk_sound_state.verbose_sound)
			printf("    Try %d to open %s\n", i, buf);
		err = snd_pcm_open (&handle, buf, SND_PCM_STREAM_PLAYBACK, mode);
		if (err >= 0)
			break;
		QuiskSleepMicrosec(500000);
	}
	if (err < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot open playback device %.40s (%.40s)\n",
				dev->name, snd_strerror (err));
		strMcpy(dev->dev_errmsg, quisk_sound_state.err_msg, QUISK_SC_SIZE);
		if (quisk_sound_state.verbose_sound)
			printf("%s\n", quisk_sound_state.err_msg);
		return 1;
	}
	dev->handle = handle;
	dev->old_key = 0;
	if ((err = snd_pcm_sw_params_malloc (&sware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot allocate software parameter structure (%s)\n",
				snd_strerror (err));
		if (quisk_sound_state.verbose_sound)
			printf("%s\n", quisk_sound_state.err_msg);
		return 1;
	}
	if ((err = snd_pcm_hw_params_malloc (&hware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot allocate hardware parameter structure (%s)\n",
				snd_strerror (err));
		if (quisk_sound_state.verbose_sound)
			printf("%s\n", quisk_sound_state.err_msg);
		snd_pcm_sw_params_free (sware);
		return 1;
	}
	if ((err = snd_pcm_hw_params_any (handle, hware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot initialize playback parameter structure (%s)\n",
				snd_strerror (err));
		goto errend;
	}
	// Get some parameters to send back
	if (snd_pcm_hw_params_get_rate_min(hware, &dev->rate_min, &dir) != 0)
		dev->rate_min = 0;	// Error
	if (snd_pcm_hw_params_get_rate_max(hware, &dev->rate_max, &dir) != 0)
		dev->rate_max = 0;	// Error
	if (snd_pcm_hw_params_get_channels_min(hware, &dev->chan_min) != 0)
		dev->chan_min= 0;	// Error
	if (snd_pcm_hw_params_get_channels_max(hware, &dev->chan_max) != 0)
		dev->chan_max= 0;	// Error
	if (quisk_sound_state.verbose_sound) {
		printf("    Sample rate min %d  max %d\n",  dev->rate_min, dev->rate_max);
		printf("    Sample rate requested %d\n", dev->sample_rate);
		printf("    Number of channels min %d  max %d\n",  dev->chan_min, dev->chan_max);
		printf("    Play channels are %d %d\n", dev->channel_I, dev->channel_Q);
	}
	// Set the playback parameters
	if (snd_pcm_hw_params_set_rate (handle, hware, dev->sample_rate, 0) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set playback rate %d",
			dev->sample_rate);
		goto errend;
	}
	if (snd_pcm_hw_params_set_access (handle, hware, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set playback access to interleaved.");
		goto errend;
	}
	if (snd_pcm_hw_params_get_channels_min(hware, &ui) != 0)
		ui = 0;	// Error
	if (dev->num_channels < (int)ui)		// increase number of channels to minimum available
		dev->num_channels = ui;
	if (snd_pcm_hw_params_set_channels (handle, hware, dev->num_channels) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set playback channels to %d",
			dev->num_channels);
		goto errend;
	}
	if (check_formats(dev, hware) == SND_PCM_FORMAT_UNKNOWN) {
		strMcpy(quisk_sound_state.msg1, dev->msg1, QUISK_SC_SIZE);
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set playback format.");
		goto errend;
	}
	if (quisk_sound_state.verbose_sound)
		printf("    %s\n", dev->msg1);
	// Set the buffer size
	frames = dev->latency_frames * 2;
	if (snd_pcm_hw_params_set_buffer_size_near (handle, hware, &frames) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Can not set playback buffer size");
		goto errend;
	}
	dev->play_buf_size = frames;
	dev->latency_frames = frames / 2;
	if ((err = snd_pcm_hw_params (handle, hware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set playback hw_params (%s)\n",
			snd_strerror (err));
		goto errend;
	}
	if ((err = snd_pcm_sw_params_current (handle, sware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot get software playback parameters (%s)\n",
				snd_strerror (err));
		goto errend;
	}
	if (snd_pcm_sw_params_set_start_threshold (handle, sware, dev->latency_frames) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set start threshold\n");
		goto errend;
	}
	if ((err = snd_pcm_sw_params (handle, sware)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot set playback sw_params (%s)\n",
			snd_strerror (err));
		goto errend;
	}
	if (quisk_sound_state.verbose_sound) {
		snd_pcm_sw_params_get_silence_threshold(sware, &frames);
		printf("    play silence threshold %d\n", (int)frames);
		snd_pcm_sw_params_get_silence_size(sware, &frames);
		printf("    play silence size %d\n", (int)frames);
		snd_pcm_sw_params_get_start_threshold(sware, &frames);
		printf("    play start threshold %d\n", (int)frames);
	}
	if ((err = snd_pcm_prepare (handle)) < 0) {
		snprintf (quisk_sound_state.err_msg, QUISK_SC_SIZE, "Cannot prepare playback interface for use (%s)\n",
			snd_strerror (err));
		goto errend;
	}
	if (quisk_sound_state.verbose_sound) {
		buffer_size = period_size = 0;
		snd_pcm_get_params (handle, &buffer_size, &period_size);
			printf("    Buffer size %d\n    Latency frames %d\n    Period size %d\n",
				(int)buffer_size, dev->latency_frames, (int)period_size);
	}
	// Success
	snd_pcm_hw_params_free (hware);
	snd_pcm_sw_params_free (sware);
	if (quisk_sound_state.verbose_sound)
		printf("*** End playback on alsa device %s %s\n", dev->name, quisk_sound_state.err_msg);
	return 0;
errend:
	snd_pcm_hw_params_free (hware);
	snd_pcm_sw_params_free (sware);
	if (quisk_sound_state.verbose_sound)
		printf("*** Error end for playback on alsa device %s %s\n", dev->name, quisk_sound_state.err_msg);
	return 1;
}

void quisk_start_sound_alsa (struct sound_dev ** pCapture, struct sound_dev ** pPlayback)
{
	struct sound_dev * pDev;

	memset(bufferz, 0, sizeof(int) * SAMP_BUFFER_SIZE);
	is_little_endian = 1;	// Test machine byte order
	if (*(char *)&is_little_endian == 1)
		is_little_endian = 1;
	else
		is_little_endian = 0;
	if (quisk_sound_state.err_msg[0])
		return;		// prior error
	// Open the alsa playback devices
	while (1) {
		pDev = *pPlayback++;
		if ( ! pDev)
			break;
		if ( ! pDev->handle && pDev->driver == DEV_DRIVER_ALSA)
			if (quisk_open_alsa_playback(pDev))
				return;		// error
	}
	// Open the alsa capture devices and start them
	while (1) {
		pDev = *pCapture++;
		if ( ! pDev)
			break;
		if ( ! pDev->handle && pDev->driver == DEV_DRIVER_ALSA) {
			if (quisk_open_alsa_capture(pDev))
				return;		// error
			if (pDev->handle)
				snd_pcm_start((snd_pcm_t *)pDev->handle);
		}
	}
}

void quisk_close_sound_alsa(struct sound_dev ** pCapture, struct sound_dev ** pPlayback)
{
	struct sound_dev * pDev;

	while (*pCapture) {
		pDev = *pCapture;
		if (pDev->handle && pDev->driver == DEV_DRIVER_ALSA) {
			snd_pcm_drop((snd_pcm_t *)pDev->handle);
			snd_pcm_close((snd_pcm_t *)pDev->handle);
			pDev->handle = NULL;
			pDev->driver = DEV_DRIVER_NONE;
		}
		pCapture++;
	}
	while (*pPlayback) {
		pDev = *pPlayback;
		if (pDev->handle && pDev->driver == DEV_DRIVER_ALSA) {
			snd_pcm_drop((snd_pcm_t *)pDev->handle);
			snd_pcm_close((snd_pcm_t *)pDev->handle);
			pDev->handle = NULL;
			pDev->driver = DEV_DRIVER_NONE;
		}
		pPlayback++;
	}
}

void quisk_alsa_mixer_set(char * card_name, int numid, PyObject * value, char * err_msg, int err_size)
// Set card card_name mixer control numid to value for integer, boolean, enum controls.
// If value is a float, interpret value as a decimal fraction of min/max.
{
	int err;
	static snd_ctl_t * handle = NULL;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_id_t * id;
	snd_ctl_elem_value_t * control;
	unsigned int idx;
	long imin, imax, tmp;
	snd_ctl_elem_type_t type;
	unsigned int count;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_value_alloca(&control);

	err_msg[0] = 0;

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_numid(id, numid);
	//snd_ctl_elem_id_set_index(id, index);
	//snd_ctl_elem_id_set_device(id, device);
	//snd_ctl_elem_id_set_subdevice(id, subdevice);
	if ( ! strncmp (card_name, "alsa:", 5)) {	// search for the name in info strings
		char buf[QUISK_SC_SIZE];
		strMcpy(buf, card_name + 5, QUISK_SC_SIZE);
		if ( ! device_list(NULL, SND_PCM_STREAM_CAPTURE, buf))	// check capture and play names
			device_list(NULL, SND_PCM_STREAM_PLAYBACK, buf);
		buf[4] = 0;		// Remove device nuumber
		err = snd_ctl_open(&handle, buf, 0);
	}
	else {		// just try to open the name
		err = snd_ctl_open(&handle, card_name, 0);
	}
	if (err < 0) {
		snprintf (err_msg, err_size, "Control %s open error: %s\n", card_name, snd_strerror(err));
		return;
	}
	snd_ctl_elem_info_set_id(info, id);
	if ((err = snd_ctl_elem_info(handle, info)) < 0) {
		snprintf (err_msg, err_size, "Cannot find the given element from control %s\n", card_name);
		return;
	}
	snd_ctl_elem_info_get_id(info, id);
	type = snd_ctl_elem_info_get_type(info);
	snd_ctl_elem_value_set_id(control, id);
	count = snd_ctl_elem_info_get_count(info);
	
	for (idx = 0; idx < count; idx++) {
		switch (type) {
		case SND_CTL_ELEM_TYPE_BOOLEAN:
			if (PyObject_IsTrue(value))
				snd_ctl_elem_value_set_boolean(control, idx, 1);
			else
				snd_ctl_elem_value_set_boolean(control, idx, 0);
			break;
		case SND_CTL_ELEM_TYPE_INTEGER:
			imin = snd_ctl_elem_info_get_min(info);
			imax = snd_ctl_elem_info_get_max(info);
			if (PyFloat_CheckExact(value)) {
				tmp = (long)(imin + (imax - imin) * PyFloat_AsDouble(value) + 0.4);
				snd_ctl_elem_value_set_integer(control, idx, tmp);
			}
			else if(PyInt_Check(value)) {
				tmp = PyInt_AsLong(value);
				snd_ctl_elem_value_set_integer(control, idx, tmp);
			}
			else {
				snprintf (err_msg, err_size, "Control %s id %d has bad value\n", card_name, numid);
			}
			break;
		case SND_CTL_ELEM_TYPE_INTEGER64:
			imin = snd_ctl_elem_info_get_min64(info);
			imax = snd_ctl_elem_info_get_max64(info);
			if (PyFloat_CheckExact(value)) {
				tmp = (long)(imin + (imax - imin) * PyFloat_AsDouble(value) + 0.4);
				snd_ctl_elem_value_set_integer64(control, idx, tmp);
			}
			else if(PyInt_Check(value)) {
				tmp = PyInt_AsLong(value);
				snd_ctl_elem_value_set_integer64(control, idx, tmp);
			}
			else {
				snprintf (err_msg, err_size, "Control %s id %d has bad value\n", card_name, numid);
			}
			break;
		case SND_CTL_ELEM_TYPE_ENUMERATED:
			if(PyInt_Check(value)) {
				tmp = PyInt_AsLong(value);
				snd_ctl_elem_value_set_enumerated(control, idx, (unsigned int)tmp);
			}
			else {
				snprintf (err_msg, err_size, "Control %s id %d has bad value\n", card_name, numid);
			}
			break;
		default:
			snprintf (err_msg, err_size, "Control %s element has unknown type\n", card_name);
			break;
		}
		if ((err = snd_ctl_elem_write(handle, control)) < 0) {
			snprintf (err_msg, err_size, "Control %s element write error: %s\n", card_name, snd_strerror(err));
			return;
		}
	}
	snd_ctl_close(handle);
	return;
}

/* The following is based on: */
//
// Programmer:    Craig Stuart Sapp <craig@ccrma.stanford.edu>
// Creation Date: Sat May  9 17:50:41 PDT 2009
// Last Modified: Sat May  9 18:14:05 PDT 2009
// Filename:      alsarawportlist.c
// Syntax:        C; ALSA 1.0
// $Smake:        gcc -o %b %f -lasound
//
// Description:	  Print available input/output MIDI ports using
//                using ALSA rawmidi interface.  Derived from 
//                amidi.c (An ALSA 1.0.19 utils program).
//

#define FRIENDLY_NAME_SIZE	256
static void midi_in_devices(PyObject * pylist, int just_names)
{	// Return a list of MIDI In devices.
	PyObject * pytup;
	int card;
	snd_ctl_t * ctl;
	snd_rawmidi_info_t * info;
	char card_name[32];
	char friendly_name[FRIENDLY_NAME_SIZE];
	int device;
	const char * name;
	int sub, subs;

	card = -1;
	snd_rawmidi_info_alloca(&info);
	while (1) {		// For all cards
		if (snd_card_next(&card) < 0 || card < 0)
			return;
		snprintf(card_name, 32, "hw:%d", card);
		if (snd_ctl_open(&ctl, card_name, 0) < 0)
			continue;
		device = -1;
		while (1) {	// For all devices
			if (snd_ctl_rawmidi_next_device(ctl, &device) < 0 || device < 0)
				break;
			snd_rawmidi_info_set_device(info, device);
			snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
			if (snd_ctl_rawmidi_info(ctl, info) == 0)
				subs = snd_rawmidi_info_get_subdevices_count(info);
			else
				subs = 0;
			for (sub = 0; sub < subs; sub++) {	// For all subdevices
				snd_rawmidi_info_set_subdevice(info, sub);
				snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
				if (snd_ctl_rawmidi_info(ctl, info) == 0) {
					name = snd_rawmidi_info_get_subdevice_name(info);
					if (name[0] == 0) {
						name = snd_rawmidi_info_get_name(info);
						if (subs == 1)
							strMcpy(friendly_name, name, FRIENDLY_NAME_SIZE);
						else
							snprintf(friendly_name, FRIENDLY_NAME_SIZE, "%s (%d)", name, sub);
					}
					else {
						strMcpy(friendly_name, name, FRIENDLY_NAME_SIZE);
					}
					if (just_names) {
						PyList_Append(pylist, PyUnicode_DecodeUTF8(friendly_name, strlen(friendly_name), "replace"));
					}
					else {
						pytup = PyTuple_New(2);
						PyList_Append(pylist, pytup);
						PyTuple_SET_ITEM(pytup, 0, PyUnicode_DecodeUTF8(friendly_name, strlen(friendly_name), "replace"));
						snprintf(card_name, 32, "hw:%d,%d,%d", card, device, sub);
						PyTuple_SET_ITEM(pytup, 1, PyUnicode_DecodeUTF8(card_name, strlen(card_name), "replace"));
					}
				}
			}
		}
	}
}

#define MIDI_MAX	6000

PyObject * quisk_alsa_control_midi(PyObject * self, PyObject * args, PyObject * keywds)
{  /* Call with keyword arguments ONLY */
	static char * kwlist[] = {"client", "device", "close_port", "get_event", "midi_cwkey_note",
					"get_in_names", "get_in_devices", NULL} ;
	int client, close_port, get_event, get_in_names, get_in_devices;
	char * device;
	static int midi_cwkey_note = -1;
	static snd_rawmidi_t * handle_in = NULL;
	PyObject * pylist;
	unsigned char ch;
	static int state = 0;
	char midi_chars[MIDI_MAX];
	int midi_length;

	client = close_port = get_event = get_in_names = get_in_devices = -1;
	device = NULL;
	if (!PyArg_ParseTupleAndKeywords (args, keywds, "|isiiiii", kwlist,
			&client, &device, &close_port, &get_event, &midi_cwkey_note, &get_in_names, &get_in_devices))
		return NULL;
	if (close_port == 1) {
		if (handle_in)
			snd_rawmidi_close(handle_in);
		handle_in = NULL;
		quisk_midi_cwkey = 0;
	}
	if (get_in_devices == 1) {	// return a list of (friendly name, device name)
		pylist = PyList_New(0);
		midi_in_devices(pylist, 0);
		return pylist;
	}
	if (get_in_names == 1) {	// return a list of friendly names
		pylist = PyList_New(0);
		midi_in_devices(pylist, 1);
		return pylist;
	}
	if (device) {		// open port
		state = 0;
		quisk_midi_cwkey = 0;
		if (snd_rawmidi_open(&handle_in, NULL, device, SND_RAWMIDI_NONBLOCK) != 0) {
			handle_in = NULL;
			printf("Failed to open MIDI device %s\n", device);
		}
		else {
			snd_rawmidi_nonblock(handle_in, 1);
			if (quisk_sound_state.verbose_sound)
				printf("Open MIDI device %s\n", device);
		}

	}
	if (get_event == 1 && handle_in) {
		midi_length = 0;
		while (snd_rawmidi_read(handle_in, &ch, 1) == 1) {
			if (midi_length < MIDI_MAX - 1)
				midi_chars[midi_length++] = ch;
			switch (state) {
			case 0:		// Wait for a status byte
				// Ignore the channel
				if (ch & 0x80) {		// This is a status byte
					if ((ch & 0xF0) == 0x80)		// Note Off
						state = 1;
					else if ((ch & 0xF0) == 0x90)	// Note On
						state = 2;
				}
				break;
			case 1:		// Note Off key number
				if (ch == midi_cwkey_note)
					quisk_midi_cwkey = 0;
				state = 0;
				break;
			case 2:		// Note On key number
				if (ch == midi_cwkey_note)
					state = 3;
				else
					state = 0;
				break;
			case 3:		// Note On velocity
				if (ch)
					quisk_midi_cwkey = 1;
				else
					quisk_midi_cwkey = 0;
				state = 0;
				break;
			}
		}
		if (midi_length > 0)
			return PyByteArray_FromStringAndSize(midi_chars, midi_length);
	}
	Py_INCREF (Py_None);
	return Py_None;
}
#else		// No Alsa available
#include <Python.h>
#include <complex.h>
#include "quisk.h"

PyObject * quisk_alsa_sound_devices(PyObject * self, PyObject * args)
{
	return quisk_dummy_sound_devices(self, args);
}

void quisk_start_sound_alsa (struct sound_dev ** pCapture, struct sound_dev ** pPlayback)
{
	struct sound_dev * pDev;
	const char * msg = "No driver support for this device";

	while (*pCapture) {
		pDev = *pCapture++;
		if (pDev->driver == DEV_DRIVER_ALSA) {
			strMcpy(pDev->dev_errmsg, msg, QUISK_SC_SIZE);
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("%s\n", msg);
		}
	}
	while (*pPlayback) {
		pDev = *pPlayback++;
		if (pDev->driver == DEV_DRIVER_ALSA) {
			strMcpy(pDev->dev_errmsg, msg, QUISK_SC_SIZE);
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("%s\n", msg);
		}
	}
}

int quisk_read_alsa(struct sound_dev * dev, complex double * cSamples)
{
	return 0;
}

void quisk_play_alsa(struct sound_dev * playdev, int nSamples, complex double * cSamples, int report_latency, double volume)
{}

void quisk_close_sound_alsa(struct sound_dev ** pCapture, struct sound_dev ** pPlayback)
{}

void quisk_alsa_sidetone(struct sound_dev * dev)
{}

void quisk_alsa_mixer_set(char * card_name, int numid, PyObject * value, char * err_msg, int err_size)
{
	err_msg[0] = 0;
}

PyObject * quisk_alsa_control_midi(PyObject * self, PyObject * args, PyObject * keywds)
{
	Py_INCREF (Py_None);
	return Py_None;
}
#endif
