/*-
 * Copyright (c) 1998 Michael Smith (msmith@freebsd.org)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>

#include <stand.h>
#include <sys/errno.h>
#include <bootstrap.h>

#include <efi.h>
#include <efilib.h>

#include "loader_efi.h"

static EFI_GUID serial = SERIAL_IO_PROTOCOL;

#define COMC_TXWAIT	0x40000		/* transmit timeout */

#ifndef	COMSPEED
#define COMSPEED	9600
#endif

struct serial {
	uint64_t	baudrate;
	uint8_t		databits;
	EFI_PARITY_TYPE	parity;
	EFI_STOP_BITS_TYPE stopbits;
	uint8_t		ignore_cd;	/* boolean */
	uint8_t		rtsdtr_off;	/* boolean */
	int		ioaddr;		/* index in handles array */
	SERIAL_IO_INTERFACE *sio;
};

static void	comc_probe(struct console *cp);
static int	comc_init(struct console *cp, int arg);
static void	comc_putchar(struct console *cp, int c);
static int	comc_getchar(struct console *cp);
static int	comc_ischar(struct console *cp);
static void	comc_setup(struct console *cp);
static char	*comc_print_mode(struct serial *sp, char *buf);
static int	comc_parse_mode(struct serial *sp, const char *value);
static int	comc_mode_set(struct env_var *, int, const void *);
static int	comc_cd_set(struct env_var *, int, const void *);
static int	comc_rtsdtr_set(struct env_var *, int, const void *);

struct console ttya = {
    "ttya",
    "serial port a",
    0,
    comc_probe,
    comc_init,
    comc_putchar,
    comc_getchar,
    comc_ischar,
    0
};

struct console ttyb = {
    "ttyb",
    "serial port b",
    0,
    comc_probe,
    comc_init,
    comc_putchar,
    comc_getchar,
    comc_ischar,
    0
};

struct console ttyc = {
    "ttyc",
    "serial port c",
    0,
    comc_probe,
    comc_init,
    comc_putchar,
    comc_getchar,
    comc_ischar,
    0
};

struct console ttyd = {
    "ttyd",
    "serial port d",
    0,
    comc_probe,
    comc_init,
    comc_putchar,
    comc_getchar,
    comc_ischar,
    0
};

EFI_STATUS
efi_serial_init(EFI_HANDLE **handlep, int *nhandles)
{
	UINTN bufsz = 0;
	EFI_STATUS status;
	EFI_HANDLE *handles;

	/*
	 * get buffer size
	 */
	*nhandles = 0;
	status = BS->LocateHandle(ByProtocol, &serial, NULL, &bufsz, handles);
	if (status != EFI_BUFFER_TOO_SMALL)
		return (status);

	if ((handles = malloc(bufsz)) == NULL)
		return (ENOMEM);

	*nhandles = (int)(bufsz/sizeof (EFI_HANDLE));
	/*
	 * get handle array
	 */
	status = BS->LocateHandle(ByProtocol, &serial, NULL, &bufsz, handles);
	if (EFI_ERROR(status)) {
		free(handles);
		*nhandles = 0;
	} else
		*handlep = handles;
	return (status);
}

static void
comc_probe(struct console *cp)
{
	EFI_STATUS status;
	struct serial *port;
	char name[20];
	char value[20];
	char *cons, *env;
	EFI_HANDLE *handles = NULL;	/* array of handles */
	int nhandles = 0;		/* number of handles in array */

	/* are we already set up? */
	if (cp->private != NULL)
		return;

	/* make sure the handles are available */
	status = efi_serial_init(&handles, &nhandles);

	cp->private = malloc(sizeof (struct serial));
	port = cp->private;
	port->baudrate = COMSPEED;

	if (strcmp(cp->c_name, "ttya") == 0)
		port->ioaddr = 0;
	else if (strcmp(cp->c_name, "ttyb") == 0)
		port->ioaddr = 1;
	else if (strcmp(cp->c_name, "ttyc") == 0)
		port->ioaddr = 2;
	else if (strcmp(cp->c_name, "ttyd") == 0)
		port->ioaddr = 3;

	if (port->ioaddr >= nhandles)
		port->ioaddr = -1;	/* invalid port */

	port->databits = 8;		/* 8,n,1 */
	port->parity = NoParity;	/* 8,n,1 */
	port->stopbits = OneStopBit;	/* 8,n,1 */
	port->ignore_cd = 1;		/* ignore cd */
	port->rtsdtr_off = 0;		/* rts-dtr is on */
	port->sio = NULL;

	if (port->ioaddr != -1) {
		status = BS->OpenProtocol(handles[port->ioaddr],
		    &serial, (void**)&port->sio, IH, NULL,
		    EFI_OPEN_PROTOCOL_GET_PROTOCOL);

		if (EFI_ERROR(status))
			port->ioaddr = -1;	/* invalid port */
	}
	if (handles != NULL)
		free(handles);

	snprintf(name, 20, "%s-mode", cp->c_name);
	env = getenv(name);

	if (env != NULL)
		(void) comc_parse_mode(port, env);

	env = comc_print_mode(port, value);
	unsetenv(name);
	env_setenv(name, EV_VOLATILE, env, comc_mode_set, env_nounset);

	snprintf(name, 20, "%s-ignore-cd", cp->c_name);
	env = getenv(name);
	if (env != NULL) {
		if (strcmp(env, "true") == 0)
			port->ignore_cd = 1;
		else if (strcmp(env, "false") == 0)
			port->ignore_cd = 0;
	}

	sprintf(value, "%s", port->ignore_cd? "true":"false");
	unsetenv(name);
	env_setenv(name, EV_VOLATILE, value, comc_cd_set, env_nounset);

	snprintf(name, 20, "%s-rts-dtr-off", cp->c_name);
	env = getenv(name);
	if (env != NULL) {
		if (strcmp(env, "true") == 0)
			port->rtsdtr_off = 1;
		else if (strcmp(env, "false") == 0)
			port->rtsdtr_off = 0;
	}

	sprintf(value, "%s", port->rtsdtr_off? "true":"false");
	unsetenv(name);
	env_setenv(name, EV_VOLATILE, value, comc_rtsdtr_set, env_nounset);
	comc_setup(cp);
}

static int
comc_init(struct console *cp, int arg __attribute((unused)))
{

	comc_setup(cp);

	if ((cp->c_flags & (C_PRESENTIN | C_PRESENTOUT)) ==
	    (C_PRESENTIN | C_PRESENTOUT))
		return (CMD_OK);
	return (CMD_ERROR);
}

static void
comc_putchar(struct console *cp, int c)
{
	int wait;
	EFI_STATUS status;
	UINTN bufsz = 1;
	char cb = c;
	struct serial *sp = cp->private;

	if (sp->sio == NULL)
		return;

	for (wait = COMC_TXWAIT; wait > 0; wait--) {
		status = sp->sio->Write(sp->sio, &bufsz, &cb);
		if (status != EFI_TIMEOUT)
			break;
	}
}

static int
comc_getchar(struct console *cp)
{
	EFI_STATUS status;
	UINTN bufsz = 1;
	char c;
	struct serial *sp = cp->private;

	if (sp->sio == NULL || !comc_ischar(cp))
		return (-1);

	status = sp->sio->Read(sp->sio, &bufsz, &c);
	if (EFI_ERROR(status) || bufsz == 0)
		return (-1);

	return (c);
}

static int
comc_ischar(struct console *cp)
{
	EFI_STATUS status;
	uint32_t control;
	struct serial *sp = cp->private;

	if (sp->sio == NULL)
		return (0);

	status = sp->sio->GetControl(sp->sio, &control);
	if (EFI_ERROR(status))
		return (0);

	return (!(status & EFI_SERIAL_INPUT_BUFFER_EMPTY));
}

static char *
comc_print_mode(struct serial *sp, char *buf)
{
	char par = 'n';
	int stop = 1;

	switch (sp->parity) {
	case NoParity: par = 'n';
		break;
	case EvenParity: par = 'e';
		break;
	case OddParity: par = 'o';
		break;
	}
	switch (sp->stopbits) {
	case OneStopBit: stop = 1;
		break;
	case TwoStopBits: stop = 2;
		break;
	}

	sprintf(buf, "%llu,%d,%c,%d,-", (unsigned long long)sp->baudrate,
	    sp->databits, par, stop);
	return (buf);
}

static int
comc_parse_mode(struct serial *sp, const char *value)
{
	int n;
	uint64_t baudrate;
	uint8_t databits = 8;
	int parity = NoParity;
	int stopbits = OneStopBit;
	char *ep;

	n = strtol(value, &ep, 0);
	if (n > 0)
		baudrate = n;
	else
		return (CMD_ERROR);

	if (*ep == ',')
		ep++;
	else
		return (CMD_ERROR);

	n = strtol(ep, &ep, 0);
	switch (n) {
	case 7: databits = 7;
		break;
	case 8: databits = 8;
		break;
	default:
		return (CMD_ERROR);
	}

	if (*ep == ',')
		ep++;
	else
		return (CMD_ERROR);

	switch (*ep++) {
	case 'n': parity = NoParity;
		break;
	case 'e': parity = EvenParity;
		break;
	case 'o': parity = OddParity;
		break;
	default:
		return (CMD_ERROR);
	}

	if (*ep == ',')
		ep++;
	else
		return (CMD_ERROR);

	switch (*ep++) {
	case '1': stopbits = OneStopBit;
		break;
	case '2': stopbits = TwoStopBits;
		break;
	default:
		return (CMD_ERROR);
	}

	/* handshake is ignored, but we check syntax anyhow */
	if (*ep == ',')
		ep++;
	else
		return (CMD_ERROR);

	switch (*ep++) {
	case '-':
	case 'h':
	case 's':
		break;
	default:
		return (CMD_ERROR);
	}

	if (*ep != '\0')
		return (CMD_ERROR);

	sp->baudrate = baudrate;
	sp->databits = databits;
	sp->parity = parity;
	sp->stopbits = stopbits;
	return (CMD_OK);
}

static struct console *
get_console(char *name)
{
	struct console *cp = NULL;

	switch(name[3]) {
	case 'a': cp = &ttya;
		break;
	case 'b': cp = &ttyb;
		break;
	case 'c': cp = &ttyc;
		break;
	case 'd': cp = &ttyd;
		break;
	}
	return (cp);
}

static int
comc_mode_set(struct env_var *ev, int flags, const void *value)
{
	struct console *cp;

	if (value == NULL)
		return (CMD_ERROR);

	if ((cp = get_console(ev->ev_name)) == NULL)
		return (CMD_ERROR);

	if (comc_parse_mode(cp->private, value) == CMD_ERROR)
		return (CMD_ERROR);

	comc_setup(cp);
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);

	return (CMD_OK);
}

static int
comc_cd_set(struct env_var *ev, int flags, const void *value)
{
	struct console *cp;
	struct serial *sp;

	if (value == NULL)
		return (CMD_ERROR);

	if ((cp = get_console(ev->ev_name)) == NULL)
		return (CMD_ERROR);

	sp = cp->private;
	if (strcmp(value, "true") == 0)
		sp->ignore_cd = 1;
	else if (strcmp(value, "false") == 0)
		sp->ignore_cd = 0;
	else
		return (CMD_ERROR);

	comc_setup(cp);
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);

	return (CMD_OK);
}

static int
comc_rtsdtr_set(struct env_var *ev, int flags, const void *value)
{
	struct console *cp;
	struct serial *sp;

	if (value == NULL)
		return (CMD_ERROR);

	if ((cp = get_console(ev->ev_name)) == NULL)
		return (CMD_ERROR);

	sp = cp->private;
	if (strcmp(value, "true") == 0)
		sp->rtsdtr_off = 1;
	else if (strcmp(value, "false") == 0)
		sp->rtsdtr_off = 0;
	else
		return (CMD_ERROR);

	comc_setup(cp);
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);

	return (CMD_OK);
}

static void
comc_setup(struct console *cp)
{
	EFI_STATUS status;
	uint32_t control;
	struct serial *sp = cp->private;

	if ((cp->c_flags & (C_ACTIVEIN | C_ACTIVEOUT)) == 0)
		return;

	/* port is not usable */
	if (sp->sio == NULL) {
		cp->c_flags &= ~(C_PRESENTIN | C_PRESENTOUT);
		return;
	}

	cp->c_flags |= (C_PRESENTIN | C_PRESENTOUT);
	status = sp->sio->Reset(sp->sio);
	if (EFI_ERROR(status)) {
		cp->c_flags &= ~(C_PRESENTIN | C_PRESENTOUT);
	}

	status = sp->sio->SetAttributes(sp->sio, sp->baudrate, 0, 0, sp->parity,
	    sp->databits, sp->stopbits);
	if (EFI_ERROR(status)) {
		cp->c_flags &= ~(C_PRESENTIN | C_PRESENTOUT);
	}

/*
	if (sp->rtsdtr_off) {
		status = sp->sio->GetControl(sp->sio, &control);
		if (EFI_ERROR(status)) {
			cp->c_flags &= ~(C_PRESENTIN | C_PRESENTOUT);
		}
		control &= ~(EFI_SERIAL_REQUEST_TO_SEND |
		    EFI_SERIAL_DATA_TERMINAL_READY);
		status = sp->sio->SetControl(sp->sio, &control);
		if (EFI_ERROR(status)) {
			cp->c_flags &= ~(C_PRESENTIN | C_PRESENTOUT);
		}
	}
*/
}
