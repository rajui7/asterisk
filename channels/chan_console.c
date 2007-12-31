/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2006 - 2007, Digium, Inc.
 *
 * Russell Bryant <russell@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! 
 * \file 
 * \brief Cross-platform console channel driver 
 *
 * \author Russell Bryant <russell@digium.com>
 *
 * \note Some of the code in this file came from chan_oss and chan_alsa.
 *       chan_oss,  Mark Spencer <markster@digium.com>
 *       chan_oss,  Luigi Rizzo
 *       chan_alsa, Matthew Fredrickson <creslin@digium.com>
 * 
 * \ingroup channel_drivers
 *
 * \note Since this works with any audio system that libportaudio supports,
 * including ALSA and OSS, this may someday deprecate chan_alsa and chan_oss.
 * However, before that can be done, it needs to *at least* have all of the
 * features that these other channel drivers have.  The features implemented
 * in at least one of the other console channel drivers that are not yet
 * implemented here are:
 *
 * - Multiple device support
 *   - with "active" CLI command
 * - Set Auto-answer from the dialplan
 * - transfer CLI command
 * - boost CLI command and .conf option
 * - console_video support
 */

/*** MODULEINFO
	<depend>portaudio</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <sys/signal.h>  /* SIGURG */

#include <portaudio.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/causes.h"
#include "asterisk/cli.h"
#include "asterisk/musiconhold.h"
#include "asterisk/callerid.h"

/*! 
 * \brief The sample rate to request from PortAudio 
 *
 * \note This should be changed to 16000 once there is a translator for going
 *       between SLINEAR and SLINEAR16.  Making it a configuration parameter
 *       would be even better, but 16 kHz should be the default.
 *
 * \note If this changes, NUM_SAMPLES will need to change, as well.
 */
#define SAMPLE_RATE      8000

/*! 
 * \brief The number of samples to configure the portaudio stream for
 *
 * 160 samples (20 ms) is the most common frame size in Asterisk.  So, the code
 * in this module reads 160 sample frames from the portaudio stream and queues
 * them up on the Asterisk channel.  Frames of any sizes can be written to a
 * portaudio stream, but the portaudio documentation does say that for high
 * performance applications, the data should be written to Pa_WriteStream in
 * the same size as what is used to initialize the stream.
 *
 * \note This will need to be dynamic once the sample rate can be something
 *       other than 8 kHz.
 */
#define NUM_SAMPLES      160

/*! \brief Mono Input */
#define INPUT_CHANNELS   1

/*! \brief Mono Output */
#define OUTPUT_CHANNELS  1

/*! 
 * \brief Maximum text message length
 * \note This should be changed if there is a common definition somewhere
 *       that defines the maximum length of a text message.
 */
#define TEXT_SIZE	256

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

/*! \brief Dance, Kirby, Dance! @{ */
#define V_BEGIN " --- <(\"<) --- "
#define V_END   " --- (>\")> ---\n"
/*! @} */

static const char config_file[] = "console.conf";

/*!
 * \brief Console pvt structure
 *
 * Currently, this is a singleton object.  However, multiple instances will be
 * needed when this module is updated for multiple device support.
 */
static struct console_pvt {
	AST_DECLARE_STRING_FIELDS(
		/*! Name of the device */
		AST_STRING_FIELD(name);
		/*! Default context for outgoing calls */
		AST_STRING_FIELD(context);
		/*! Default extension for outgoing calls */
		AST_STRING_FIELD(exten);
		/*! Default CallerID number */
		AST_STRING_FIELD(cid_num);
		/*! Default CallerID name */
		AST_STRING_FIELD(cid_name);
		/*! Default MOH class to listen to, if:
		 *    - No MOH class set on the channel
		 *    - Peer channel putting this device on hold did not suggest a class */
		AST_STRING_FIELD(mohinterpret);
		/*! Default language */
		AST_STRING_FIELD(language);
	);
	/*! Current channel for this device */
	struct ast_channel *owner;
	/*! Current PortAudio stream for this device */
	PaStream *stream;
	/*! A frame for preparing to queue on to the channel */
	struct ast_frame fr;
	/*! Running = 1, Not running = 0 */
	unsigned int streamstate:1;
	/*! On-hook = 0, Off-hook = 1 */
	unsigned int hookstate:1;
	/*! Unmuted = 0, Muted = 1 */
	unsigned int muted:1;
	/*! Automatically answer incoming calls */
	unsigned int autoanswer:1;
	/*! Ignore context in the console dial CLI command */
	unsigned int overridecontext:1;
	/*! Lock to protect data in this struct */
	ast_mutex_t __lock;
	/*! ID for the stream monitor thread */
	pthread_t thread;
} console_pvt = {
	.__lock = AST_MUTEX_INIT_VALUE,
	.thread = AST_PTHREADT_NULL,
};

/*! 
 * \brief Global jitterbuffer configuration 
 *
 * \note Disabled by default.
 */
static struct ast_jb_conf default_jbconf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = ""
};
static struct ast_jb_conf global_jbconf;

/*! Channel Technology Callbacks @{ */
static struct ast_channel *console_request(const char *type, int format, 
	void *data, int *cause);
static int console_digit_begin(struct ast_channel *c, char digit);
static int console_digit_end(struct ast_channel *c, char digit, unsigned int duration);
static int console_text(struct ast_channel *c, const char *text);
static int console_hangup(struct ast_channel *c);
static int console_answer(struct ast_channel *c);
static struct ast_frame *console_read(struct ast_channel *chan);
static int console_call(struct ast_channel *c, char *dest, int timeout);
static int console_write(struct ast_channel *chan, struct ast_frame *f);
static int console_indicate(struct ast_channel *chan, int cond, 
	const void *data, size_t datalen);
static int console_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
/*! @} */

/*!
 * \brief Formats natively supported by this module.
 *
 * \note Once 16 kHz is supported, AST_FORMAT_SLINEAR16 needs to be added.
 */
#define SUPPORTED_FORMATS ( AST_FORMAT_SLINEAR )

static const struct ast_channel_tech console_tech = {
	.type = "Console",
	.description = "Console Channel Driver",
	.capabilities = SUPPORTED_FORMATS,
	.requester = console_request,
	.send_digit_begin = console_digit_begin,
	.send_digit_end = console_digit_end,
	.send_text = console_text,
	.hangup = console_hangup,
	.answer = console_answer,
	.read = console_read,
	.call = console_call,
	.write = console_write,
	.indicate = console_indicate,
	.fixup = console_fixup,
};

/*! \brief lock a console_pvt struct */
#define console_pvt_lock(pvt) ast_mutex_lock(&(pvt)->__lock)

/*! \brief unlock a console_pvt struct */
#define console_pvt_unlock(pvt) ast_mutex_unlock(&(pvt)->__lock)

/*!
 * \brief Stream monitor thread 
 *
 * \arg data A pointer to the console_pvt structure that contains the portaudio
 *      stream that needs to be monitored.
 *
 * This function runs in its own thread to monitor data coming in from a
 * portaudio stream.  When enough data is available, it is queued up to
 * be read from the Asterisk channel.
 */
static void *stream_monitor(void *data)
{
	struct console_pvt *pvt = data;
	char buf[NUM_SAMPLES * sizeof(int16_t)];
	PaError res;
	struct ast_frame f = {
		.frametype = AST_FRAME_VOICE,
		.subclass = AST_FORMAT_SLINEAR,
		.src = "console_stream_monitor",
		.data = buf,
		.datalen = sizeof(buf),
		.samples = sizeof(buf) / sizeof(int16_t),
	};

	for (;;) {
		pthread_testcancel();
		res = Pa_ReadStream(pvt->stream, buf, sizeof(buf) / sizeof(int16_t));
		pthread_testcancel();

		if (res == paNoError)
			ast_queue_frame(pvt->owner, &f);
	}

	return NULL;
}

static int start_stream(struct console_pvt *pvt)
{
	PaError res;
	int ret_val = 0;

	console_pvt_lock(pvt);

	if (pvt->streamstate)
		goto return_unlock;

	pvt->streamstate = 1;
	ast_debug(1, "Starting stream\n");

	res = Pa_OpenDefaultStream(&pvt->stream, INPUT_CHANNELS, OUTPUT_CHANNELS, 
		paInt16, SAMPLE_RATE, NUM_SAMPLES, NULL, NULL);
	if (res != paNoError) {
		ast_log(LOG_WARNING, "Failed to open default audio device - (%d) %s\n",
			res, Pa_GetErrorText(res));
		ret_val = -1;
		goto return_unlock;
	}

	res = Pa_StartStream(pvt->stream);
	if (res != paNoError) {
		ast_log(LOG_WARNING, "Failed to start stream - (%d) %s\n",
			res, Pa_GetErrorText(res));
		ret_val = -1;
		goto return_unlock;
	}

	if (ast_pthread_create_background(&pvt->thread, NULL, stream_monitor, pvt)) {
		ast_log(LOG_ERROR, "Failed to start stream monitor thread\n");
		ret_val = -1;
	}

return_unlock:
	console_pvt_unlock(pvt);

	return ret_val;
}

static int stop_stream(struct console_pvt *pvt)
{
	if (!pvt->streamstate)
		return 0;

	pthread_cancel(pvt->thread);
	pthread_kill(pvt->thread, SIGURG);
	pthread_join(pvt->thread, NULL);

	console_pvt_lock(pvt);
	Pa_AbortStream(pvt->stream);
	Pa_CloseStream(pvt->stream);
	pvt->stream = NULL;
	pvt->streamstate = 0;
	console_pvt_unlock(pvt);

	return 0;
}

/*!
 * \note Called with the pvt struct locked
 */
static struct ast_channel *console_new(struct console_pvt *pvt, const char *ext, const char *ctx, int state)
{
	struct ast_channel *chan;

	if (!(chan = ast_channel_alloc(1, state, pvt->cid_num, pvt->cid_name, NULL, 
		ext, ctx, 0, "Console/%s", pvt->name))) {
		return NULL;
	}

	chan->tech = &console_tech;
	chan->nativeformats = AST_FORMAT_SLINEAR;
	chan->readformat = AST_FORMAT_SLINEAR;
	chan->writeformat = AST_FORMAT_SLINEAR;
	chan->tech_pvt = pvt;

	pvt->owner = chan;

	if (!ast_strlen_zero(pvt->language))
		ast_string_field_set(chan, language, pvt->language);

	ast_jb_configure(chan, &global_jbconf);

	if (state != AST_STATE_DOWN) {
		if (ast_pbx_start(chan)) {
			chan->hangupcause = AST_CAUSE_SWITCH_CONGESTION;
			ast_hangup(chan);
			chan = NULL;
		} else
			start_stream(pvt);
	}

	return chan;
}

static struct ast_channel *console_request(const char *type, int format, void *data, int *cause)
{
	int oldformat = format;
	struct ast_channel *chan;
	struct console_pvt *pvt = &console_pvt;

	format &= SUPPORTED_FORMATS;
	if (!format) {
		ast_log(LOG_NOTICE, "Channel requested with unsupported format(s): '%d'\n", oldformat);
		return NULL;
	}

	if (pvt->owner) {
		ast_log(LOG_NOTICE, "Console channel already active!\n");
		*cause = AST_CAUSE_BUSY;
		return NULL;
	}

	console_pvt_lock(pvt);
	chan = console_new(pvt, NULL, NULL, AST_STATE_DOWN);
	console_pvt_unlock(pvt);

	if (!chan)
		ast_log(LOG_WARNING, "Unable to create new Console channel!\n");

	return chan;
}

static int console_digit_begin(struct ast_channel *c, char digit)
{
	ast_verb(1, V_BEGIN "Console Received Beginning of Digit %c" V_END, digit);

	return -1; /* non-zero to request inband audio */
}

static int console_digit_end(struct ast_channel *c, char digit, unsigned int duration)
{
	ast_verb(1, V_BEGIN "Console Received End of Digit %c (duration %u)" V_END, 
		digit, duration);

	return -1; /* non-zero to request inband audio */
}

static int console_text(struct ast_channel *c, const char *text)
{
	ast_verb(1, V_BEGIN "Console Received Text '%s'" V_END, text);

	return 0;
}

static int console_hangup(struct ast_channel *c)
{
	struct console_pvt *pvt = &console_pvt;

	ast_verb(1, V_BEGIN "Hangup on Console" V_END);

	pvt->hookstate = 0;
	c->tech_pvt = NULL;
	pvt->owner = NULL;

	stop_stream(pvt);

	return 0;
}

static int console_answer(struct ast_channel *c)
{
	struct console_pvt *pvt = &console_pvt;

	ast_verb(1, V_BEGIN "Call from Console has been Answered" V_END);

	ast_setstate(c, AST_STATE_UP);

	return start_stream(pvt);
}

/*
 * \brief Implementation of the ast_channel_tech read() callback
 *
 * Calling this function is harmless.  However, if it does get called, it
 * is an indication that something weird happened that really shouldn't
 * have and is worth looking into.
 * 
 * Why should this function not get called?  Well, let me explain.  There are
 * a couple of ways to pass on audio that has come from this channel.  The way
 * that this channel driver uses is that once the audio is available, it is
 * wrapped in an ast_frame and queued onto the channel using ast_queue_frame().
 *
 * The other method would be signalling to the core that there is audio waiting,
 * and that it needs to call the channel's read() callback to get it.  The way
 * the channel gets signalled is that one or more file descriptors are placed
 * in the fds array on the ast_channel which the core will poll() on.  When the
 * fd indicates that input is available, the read() callback is called.  This
 * is especially useful when there is a dedicated file descriptor where the
 * audio is read from.  An example would be the socket for an RTP stream.
 */
static struct ast_frame *console_read(struct ast_channel *chan)
{
	ast_debug(1, "I should not be called ...\n");

	return &ast_null_frame;
}

static int console_call(struct ast_channel *c, char *dest, int timeout)
{
	struct ast_frame f = { 0, };
	struct console_pvt *pvt = &console_pvt;

	ast_verb(1, V_BEGIN "Call to device '%s' on console from '%s' <%s>" V_END,
		dest, c->cid.cid_name, c->cid.cid_num);

	console_pvt_lock(pvt);

	if (pvt->autoanswer) {
		ast_verb(1, V_BEGIN "Auto-answered" V_END);
		pvt->hookstate = 1;
		f.frametype = AST_FRAME_CONTROL;
		f.subclass = AST_CONTROL_ANSWER;
	} else {
		ast_verb(1, V_BEGIN "Type 'answer' to answer, or use 'autoanswer' "
				"for future calls" V_END);
		f.frametype = AST_FRAME_CONTROL;
		f.subclass = AST_CONTROL_RINGING;
	}

	console_pvt_unlock(pvt);

	ast_queue_frame(c, &f);

	return start_stream(pvt);
}

static int console_write(struct ast_channel *chan, struct ast_frame *f)
{
	struct console_pvt *pvt = &console_pvt;

	Pa_WriteStream(pvt->stream, f->data, f->samples);

	return 0;
}

static int console_indicate(struct ast_channel *chan, int cond, const void *data, size_t datalen)
{
	struct console_pvt *pvt = chan->tech_pvt;
	int res = 0;

	switch (cond) {
	case AST_CONTROL_BUSY:
	case AST_CONTROL_CONGESTION:
	case AST_CONTROL_RINGING:
		res = -1;  /* Ask for inband indications */
		break;
	case AST_CONTROL_PROGRESS:
	case AST_CONTROL_PROCEEDING:
	case AST_CONTROL_VIDUPDATE:
	case -1:
		break;
	case AST_CONTROL_HOLD:
		ast_verb(1, V_BEGIN "Console Has Been Placed on Hold" V_END);
		ast_moh_start(chan, data, pvt->mohinterpret);
		break;
	case AST_CONTROL_UNHOLD:
		ast_verb(1, V_BEGIN "Console Has Been Retrieved from Hold" V_END);
		ast_moh_stop(chan);
		break;
	default:
		ast_log(LOG_WARNING, "Don't know how to display condition %d on %s\n", 
			cond, chan->name);
		/* The core will play inband indications for us if appropriate */
		res = -1;
	}

	return res;
}

static int console_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct console_pvt *pvt = &console_pvt;

	pvt->owner = newchan;

	return 0;
}

/*!
 * split a string in extension-context, returns pointers to malloc'ed
 * strings.
 * If we do not have 'overridecontext' then the last @ is considered as
 * a context separator, and the context is overridden.
 * This is usually not very necessary as you can play with the dialplan,
 * and it is nice not to need it because you have '@' in SIP addresses.
 * Return value is the buffer address.
 *
 * \note came from chan_oss
 */
static char *ast_ext_ctx(struct console_pvt *pvt, const char *src, char **ext, char **ctx)
{
	if (ext == NULL || ctx == NULL)
		return NULL;			/* error */

	*ext = *ctx = NULL;

	if (src && *src != '\0')
		*ext = ast_strdup(src);

	if (*ext == NULL)
		return NULL;

	if (!pvt->overridecontext) {
		/* parse from the right */
		*ctx = strrchr(*ext, '@');
		if (*ctx)
			*(*ctx)++ = '\0';
	}

	return *ext;
}

static char *cli_console_autoanswer(struct ast_cli_entry *e, int cmd, 
	struct ast_cli_args *a)
{
	struct console_pvt *pvt = &console_pvt;

	switch (cmd) {
	case CLI_INIT:
		e->command = "console set autoanswer [on|off]";
		e->usage =
			"Usage: console set autoanswer [on|off]\n"
			"       Enables or disables autoanswer feature.  If used without\n"
			"       argument, displays the current on/off status of autoanswer.\n"
			"       The default value of autoanswer is in 'oss.conf'.\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == e->args - 1) {
		ast_cli(a->fd, "Auto answer is %s.\n", pvt->autoanswer ? "on" : "off");
		return CLI_SUCCESS;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (!pvt) {
		ast_log(LOG_WARNING, "Cannot find device %s (should not happen!)\n",
			pvt->name);
		return CLI_FAILURE;
	}

	if (!strcasecmp(a->argv[e->args-1], "on"))
		pvt->autoanswer = 1;
	else if (!strcasecmp(a->argv[e->args - 1], "off"))
		pvt->autoanswer = 0;
	else
		return CLI_SHOWUSAGE;

	return CLI_SUCCESS;
}

static char *cli_console_flash(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_FLASH };
	struct console_pvt *pvt = &console_pvt;

	if (cmd == CLI_INIT) {
		e->command = "console flash";
		e->usage =
			"Usage: console flash\n"
			"       Flashes the call currently placed on the console.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (!pvt->owner) {
		ast_cli(a->fd, "No call to flash\n");
		return CLI_FAILURE;
	}

	pvt->hookstate = 0;

	ast_queue_frame(pvt->owner, &f);

	return CLI_SUCCESS;
}

static char *cli_console_dial(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *s = NULL;
	const char *mye = NULL, *myc = NULL; 
	struct console_pvt *pvt = &console_pvt;

	if (cmd == CLI_INIT) {
		e->command = "console dial";
		e->usage =
			"Usage: console dial [extension[@context]]\n"
			"       Dials a given extension (and context if specified)\n";
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;

	if (a->argc > e->args + 1)
		return CLI_SHOWUSAGE;

	if (pvt->owner) {	/* already in a call */
		int i;
		struct ast_frame f = { AST_FRAME_DTMF, 0 };

		if (a->argc == e->args) {	/* argument is mandatory here */
			ast_cli(a->fd, "Already in a call. You can only dial digits until you hangup.\n");
			return CLI_FAILURE;
		}
		s = a->argv[e->args];
		/* send the string one char at a time */
		for (i = 0; i < strlen(s); i++) {
			f.subclass = s[i];
			ast_queue_frame(pvt->owner, &f);
		}
		return CLI_SUCCESS;
	}

	/* if we have an argument split it into extension and context */
	if (a->argc == e->args + 1) {
		char *ext = NULL, *con = NULL;
		s = ast_ext_ctx(pvt, a->argv[e->args], &ext, &con);
		ast_debug(1, "provided '%s', exten '%s' context '%s'\n", 
			a->argv[e->args], mye, myc);
		mye = ext;
		myc = con;
	}

	/* supply default values if needed */
	if (ast_strlen_zero(mye))
		mye = pvt->exten;
	if (ast_strlen_zero(myc))
		myc = pvt->context;

	if (ast_exists_extension(NULL, myc, mye, 1, NULL)) {
		console_pvt_lock(pvt);
		pvt->hookstate = 1;
		console_new(pvt, mye, myc, AST_STATE_RINGING);
		console_pvt_unlock(pvt);
	} else
		ast_cli(a->fd, "No such extension '%s' in context '%s'\n", mye, myc);

	if (s)
		free(s);

	return CLI_SUCCESS;
}

static char *cli_console_hangup(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct console_pvt *pvt = &console_pvt;

	if (cmd == CLI_INIT) {
		e->command = "console hangup";
		e->usage =
			"Usage: console hangup\n"
			"       Hangs up any call currently placed on the console.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (!pvt->owner && !pvt->hookstate) {
		ast_cli(a->fd, "No call to hang up\n");
		return CLI_FAILURE;
	}

	pvt->hookstate = 0;
	if (pvt->owner)
		ast_queue_hangup(pvt->owner);

	return CLI_SUCCESS;
}

static char *cli_console_mute(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *s;
	struct console_pvt *pvt = &console_pvt;
	
	if (cmd == CLI_INIT) {
		e->command = "console {mute|unmute}";
		e->usage =
			"Usage: console {mute|unmute}\n"
			"       Mute/unmute the microphone.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	s = a->argv[e->args-1];
	if (!strcasecmp(s, "mute"))
		pvt->muted = 1;
	else if (!strcasecmp(s, "unmute"))
		pvt->muted = 0;
	else
		return CLI_SHOWUSAGE;

	ast_verb(1, V_BEGIN "The Console is now %s" V_END, 
		pvt->muted ? "Muted" : "Unmuted");

	return CLI_SUCCESS;
}

static char *cli_list_devices(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	PaDeviceIndex index, num, def_input, def_output;

	if (cmd == CLI_INIT) {
		e->command = "console list devices";
		e->usage =
			"Usage: console list devices\n"
			"       List all available devices.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	ast_cli(a->fd, "Available Devices:\n---------------------------------\n");

	num = Pa_GetDeviceCount();
	if (!num) {
		ast_cli(a->fd, "(None)\n");
		return CLI_SUCCESS;
	}

	def_input = Pa_GetDefaultInputDevice();
	def_output = Pa_GetDefaultOutputDevice();
	for (index = 0; index < num; index++) {
		const PaDeviceInfo *dev = Pa_GetDeviceInfo(index);
		if (!dev)
			continue;
		ast_cli(a->fd, "Device Name: %s\n", dev->name);
		if (index == def_input)
			ast_cli(a->fd, "    ---> Default Input Device\n");
		if (index == def_output)
			ast_cli(a->fd, "    ---> Default Output Device\n");
	}

	return CLI_SUCCESS;
}

/*!
 * \brief answer command from the console
 */
static char *cli_console_answer(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_ANSWER };
	struct console_pvt *pvt = &console_pvt;

	switch (cmd) {
	case CLI_INIT:
		e->command = "console answer";
		e->usage =
			"Usage: console answer\n"
			"       Answers an incoming call on the console channel.\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;	/* no completion */
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (!pvt->owner) {
		ast_cli(a->fd, "No one is calling us\n");
		return CLI_FAILURE;
	}

	pvt->hookstate = 1;
	ast_queue_frame(pvt->owner, &f);

	return CLI_SUCCESS;
}

/*!
 * \brief Console send text CLI command
 *
 * \note concatenate all arguments into a single string. argv is NULL-terminated
 * so we can use it right away
 */
static char *cli_console_sendtext(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char buf[TEXT_SIZE];
	struct console_pvt *pvt = &console_pvt;
	struct ast_frame f = {
		.frametype = AST_FRAME_TEXT,
		.data = buf,
		.src = "console_send_text",
	};
	int len;

	if (cmd == CLI_INIT) {
		e->command = "console send text";
		e->usage =
			"Usage: console send text <message>\n"
			"       Sends a text message for display on the remote terminal.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;

	if (a->argc < e->args + 1)
		return CLI_SHOWUSAGE;

	if (!pvt->owner) {
		ast_cli(a->fd, "Not in a call\n");
		return CLI_FAILURE;
	}

	ast_join(buf, sizeof(buf) - 1, a->argv + e->args);
	if (ast_strlen_zero(buf))
		return CLI_SHOWUSAGE;

	len = strlen(buf);
	buf[len] = '\n';
	f.datalen = len + 1;

	ast_queue_frame(pvt->owner, &f);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_console[] = {
	AST_CLI_DEFINE(cli_console_dial,       "Dial an extension from the console"),
	AST_CLI_DEFINE(cli_console_hangup,     "Hangup a call on the console"),
	AST_CLI_DEFINE(cli_console_mute,       "Disable/Enable mic input"),
	AST_CLI_DEFINE(cli_console_answer,     "Answer an incoming console call"),
	AST_CLI_DEFINE(cli_console_sendtext,   "Send text to a connected party"),
	AST_CLI_DEFINE(cli_console_flash,      "Send a flash to the connected party"),
	AST_CLI_DEFINE(cli_console_autoanswer, "Turn autoanswer on or off"),
	AST_CLI_DEFINE(cli_list_devices,       "List available devices"),
};

/*!
 * \brief Set default values for a pvt struct
 *
 * \note This function expects the pvt lock to be held.
 */
static void set_pvt_defaults(struct console_pvt *pvt, int reload)
{
	if (!reload) {
		/* This should be changed for multiple device support.  Right now,
		 * there is no way to change the name of a device.  The default
		 * input and output sound devices are the only ones supported. */
		ast_string_field_set(pvt, name, "default");
	}

	ast_string_field_set(pvt, mohinterpret, "default");
	ast_string_field_set(pvt, context, "default");
	ast_string_field_set(pvt, exten, "s");
	ast_string_field_set(pvt, language, "");
	ast_string_field_set(pvt, cid_num, "");
	ast_string_field_set(pvt, cid_name, "");

	pvt->overridecontext = 0;
	pvt->autoanswer = 0;
}

static void store_callerid(struct console_pvt *pvt, const char *value)
{
	char cid_name[256];
	char cid_num[256];

	ast_callerid_split(value, cid_name, sizeof(cid_name), 
		cid_num, sizeof(cid_num));

	ast_string_field_set(pvt, cid_name, cid_name);
	ast_string_field_set(pvt, cid_num, cid_num);
}

/*!
 * \brief Store a configuration parameter in a pvt struct
 *
 * \note This function expects the pvt lock to be held.
 */
static void store_config_core(struct console_pvt *pvt, const char *var, const char *value)
{
	if (!ast_jb_read_conf(&global_jbconf, var, value))
		return;

	CV_START(var, value);

	CV_STRFIELD("context", pvt, context);
	CV_STRFIELD("extension", pvt, exten);
	CV_STRFIELD("mohinterpret", pvt, mohinterpret);
	CV_STRFIELD("language", pvt, language);
	CV_F("callerid", store_callerid(pvt, value));
	CV_BOOL("overridecontext", pvt->overridecontext);
	CV_BOOL("autoanswer", pvt->autoanswer);
	
	ast_log(LOG_WARNING, "Unknown option '%s'\n", var);

	CV_END;
}

/*!
 * \brief Load the configuration
 * \param reload if this was called due to a reload
 * \retval 0 succcess
 * \retval -1 failure
 */
static int load_config(int reload)
{
	struct ast_config *cfg;
	struct ast_variable *v;
	struct console_pvt *pvt = &console_pvt;
	struct ast_flags config_flags = { 0 };
	int res = -1;

	/* default values */
	memcpy(&global_jbconf, &default_jbconf, sizeof(global_jbconf));

	console_pvt_lock(pvt);

	set_pvt_defaults(pvt, reload);

	if (!(cfg = ast_config_load(config_file, config_flags))) {
		ast_log(LOG_NOTICE, "Unable to open configuration file %s!\n", config_file);
		goto return_unlock;
	}

	for (v = ast_variable_browse(cfg, "general"); v; v = v->next)
		store_config_core(pvt, v->name, v->value);

	ast_config_destroy(cfg);

	res = 0;

return_unlock:
	console_pvt_unlock(pvt);
	return res;
}

static int init_pvt(struct console_pvt *pvt)
{
	if (ast_string_field_init(pvt, 32))
		return -1;
	
	if (ast_mutex_init(&pvt->__lock)) {
		ast_log(LOG_ERROR, "Failed to initialize mutex\n");
		return -1;
	}

	return 0;
}

static void destroy_pvt(struct console_pvt *pvt)
{
	ast_string_field_free_memory(pvt);
	
	ast_mutex_destroy(&pvt->__lock);
}

static int unload_module(void)
{
	struct console_pvt *pvt = &console_pvt;

	if (pvt->hookstate)
		stop_stream(pvt);

	Pa_Terminate();

	ast_channel_unregister(&console_tech);
	ast_cli_unregister_multiple(cli_console, ARRAY_LEN(cli_console));

	destroy_pvt(pvt);

	return 0;
}

static int load_module(void)
{
	PaError res;
	struct console_pvt *pvt = &console_pvt;

	if (init_pvt(pvt))
		goto return_error;

	if (load_config(0))
		goto return_error;

	res = Pa_Initialize();
	if (res != paNoError) {
		ast_log(LOG_WARNING, "Failed to initialize audio system - (%d) %s\n",
			res, Pa_GetErrorText(res));
		goto return_error_pa_init;
	}

	if (ast_channel_register(&console_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel type 'Console'\n");
		goto return_error_chan_reg;
	}

	if (ast_cli_register_multiple(cli_console, ARRAY_LEN(cli_console)))
		goto return_error_cli_reg;

	return AST_MODULE_LOAD_SUCCESS;

return_error_cli_reg:
	ast_cli_unregister_multiple(cli_console, ARRAY_LEN(cli_console));
return_error_chan_reg:
	ast_channel_unregister(&console_tech);
return_error_pa_init:
	Pa_Terminate();
return_error:
	destroy_pvt(pvt);

	return AST_MODULE_LOAD_DECLINE;
}

static int reload(void)
{
	return load_config(1);
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Console Channel Driver",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
);
