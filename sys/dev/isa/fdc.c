/*	$OpenBSD: fdc.c,v 1.2 1996/10/16 12:46:25 deraadt Exp $	*/
/*	$NetBSD: fd.c,v 1.90 1996/05/12 23:12:03 mycroft Exp $	*/

/*-
 * Copyright (c) 1993, 1994, 1995 Charles Hannum.
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Don Ahn.
 *
 * Portions Copyright (c) 1993, 1994 by
 *  jc@irbs.UUCP (John Capo)
 *  vak@zebub.msk.su (Serge Vakulenko)
 *  ache@astral.msk.su (Andrew A. Chernov)
 *  joerg_wunsch@uriah.sax.de (Joerg Wunsch)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)fd.c	7.4 (Berkeley) 5/25/91
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/device.h>
#include <sys/disklabel.h>
#include <sys/dkstat.h>
#include <sys/disk.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/uio.h>
#include <sys/mtio.h>
#include <sys/syslog.h>
#include <sys/queue.h>

#include <machine/cpu.h>
#include <machine/bus.h>
#include <machine/conf.h>
#include <machine/intr.h>
#include <machine/ioctl_fd.h>

#include <dev/isa/isavar.h>
#include <dev/isa/isadmavar.h>
#include <i386/isa/fdreg.h>

#include <dev/ic/mc146818reg.h>			/* for NVRAM access */
#include <i386/isa/nvram.h>

#include <i386/isa/fdlink.h>

#include "fd.h"

/* controller driver configuration */
int fdcprobe __P((struct device *, void *, void *));
#ifdef NEWCONFIG
void fdcforceintr __P((void *));
#endif
void fdcattach __P((struct device *, struct device *, void *));

struct cfattach fdc_ca = {
	sizeof(struct fdc_softc), fdcprobe, fdcattach
};

struct cfdriver fdc_cd = {
	NULL, "fdc", DV_DULL
};

int fddprint __P((void *, char *));
int fdcintr __P((void *));

int
fdcprobe(parent, match, aux)
	struct device *parent;
	void *match, *aux;
{
	register struct isa_attach_args *ia = aux;
	bus_chipset_tag_t bc;
	bus_io_handle_t ioh;
	int rv;

	bc = ia->ia_bc;
	rv = 0;

	/* Map the i/o space. */
	if (bus_io_map(bc, ia->ia_iobase, FDC_NPORT, &ioh))
		return 0;

	/* reset */
	bus_io_write_1(bc, ioh, fdout, 0);
	delay(100);
	bus_io_write_1(bc, ioh, fdout, FDO_FRST);

	/* see if it can handle a command */
	if (out_fdc(bc, ioh, NE7CMD_SPECIFY) < 0)
		goto out;
	out_fdc(bc, ioh, 0xdf);
	out_fdc(bc, ioh, 2);

#ifdef NEWCONFIG
	if (ia->ia_iobase == IOBASEUNK || ia->ia_drq == DRQUNK)
		return 0;

	if (ia->ia_irq == IRQUNK) {
		ia->ia_irq = isa_discoverintr(fdcforceintr, aux);
		if (ia->ia_irq == IRQNONE)
			goto out;

		/* reset it again */
		bus_io_write_1(bc, ioh, fdout, 0);
		delay(100);
		bus_io_write_1(bc, ioh, fdout, FDO_FRST);
	}
#endif

	rv = 1;
	ia->ia_iosize = FDC_NPORT;
	ia->ia_msize = 0;

 out:
	bus_io_unmap(bc, ioh, FDC_NPORT);
	return rv;
}

#ifdef NEWCONFIG
/*
 * XXX This is broken, and needs fixing.  In general, the interface needs
 * XXX to change.
 */
void
fdcforceintr(aux)
	void *aux;
{
	struct isa_attach_args *ia = aux;
	int iobase = ia->ia_iobase;

	/* the motor is off; this should generate an error with or
	   without a disk drive present */
	out_fdc(bc, ioh, NE7CMD_SEEK);
	out_fdc(bc, ioh, 0);
	out_fdc(bc, ioh, 0);
}
#endif

void
fdcattach(parent, self, aux)
	struct device *parent, *self;
	void *aux;
{
	struct fdc_softc *fdc = (void *)self;
	bus_chipset_tag_t bc;
	bus_io_handle_t ioh;
	struct isa_attach_args *ia = aux;
	struct fdc_attach_args fa;
	int type;

	bc = ia->ia_bc;

	/* Re-map the I/O space. */
	if (bus_io_map(bc, ia->ia_iobase, FDC_NPORT, &ioh))
		panic("fdcattach: couldn't map I/O ports");

	fdc->sc_bc = bc;
	fdc->sc_ioh = ioh;

	fdc->sc_drq = ia->ia_drq;
	fdc->sc_state = DEVIDLE;
	TAILQ_INIT(&fdc->sc_drives);

	printf("\n");

#ifdef NEWCONFIG
	at_setup_dmachan(fdc->sc_drq, FDC_MAXIOSIZE);
	isa_establish(&fdc->sc_id, &fdc->sc_dev);
#endif
	fdc->sc_ih = isa_intr_establish(ia->ia_ic, ia->ia_irq, IST_EDGE,
	    IPL_BIO, fdcintr, fdc, fdc->sc_dev.dv_xname);

	/*
	 * The NVRAM info only tells us about the first two disks on the
	 * `primary' floppy controller.
	 */
	if (fdc->sc_dev.dv_unit == 0)
		type = mc146818_read(NULL, NVRAM_DISKETTE); /* XXX softc */
	else
		type = -1;

	/* physical limit: four drives per controller. */
	for (fa.fa_drive = 0; fa.fa_drive < 4; fa.fa_drive++) {
		fa.fa_flags = 0;
#if NFD > 0
		if (type >= 0 && fa.fa_drive < 2)
			fa.fa_deftype = fd_nvtotype(fdc->sc_dev.dv_xname,
			    type, fa.fa_drive);
		else
#endif
			fa.fa_deftype = NULL;		/* unknown */
		(void)config_found(self, (void *)&fa, fddprint);
	}
}

/*
 * Print the location of a disk/tape drive (called just before attaching the
 * the drive).  If `fdc' is not NULL, the drive was found but was not
 * in the system config file; print the drive name as well.
 * Return QUIET (config_find ignores this if the device was configured) to
 * avoid printing `fdN not configured' messages.
 */
int
fddprint(aux, fdc)
	void *aux;
	char *fdc;
{
	register struct fdc_attach_args *fa = aux;

	if (!fdc)
		printf(" drive %d", fa->fa_drive);
	return QUIET;
}

int
fdcresult(fdc)
	struct fdc_softc *fdc;
{
	bus_chipset_tag_t bc = fdc->sc_bc;
	bus_io_handle_t ioh = fdc->sc_ioh;
	u_char i;
	int j = 100000,
	    n = 0;

	for (; j; j--) {
		i = bus_io_read_1(bc, ioh, fdsts) &
		    (NE7_DIO | NE7_RQM | NE7_CB);
		if (i == NE7_RQM)
			return n;
		if (i == (NE7_DIO | NE7_RQM | NE7_CB)) {
			if (n >= sizeof(fdc->sc_status)) {
				log(LOG_ERR, "fdcresult: overrun\n");
				return -1;
			}
			fdc->sc_status[n++] = bus_io_read_1(bc, ioh, fddata);
		}
	}
	log(LOG_ERR, "fdcresult: timeout\n");
	return -1;
}

int
out_fdc(bc, ioh, x)
	bus_chipset_tag_t bc;
	bus_io_handle_t ioh;
	u_char x;
{
	int i = 100000;

	while ((bus_io_read_1(bc, ioh, fdsts) & NE7_DIO) && i-- > 0);
	if (i <= 0)
		return -1;
	while ((bus_io_read_1(bc, ioh, fdsts) & NE7_RQM) == 0 && i-- > 0);
	if (i <= 0)
		return -1;
	bus_io_write_1(bc, ioh, fddata, x);
	return 0;
}

void
fdcstart(fdc)
	struct fdc_softc *fdc;
{

#ifdef DIAGNOSTIC
	/* only got here if controller's drive queue was inactive; should
	   be in idle state */
	if (fdc->sc_state != DEVIDLE) {
		printf("fdcstart: not idle\n");
		return;
	}
#endif
	(void) fdcintr(fdc);
}

void
fdcstatus(dv, n, s)
	struct device *dv;
	int n;
	char *s;
{
	struct fdc_softc *fdc = (void *)dv->dv_parent;

	if (n == 0) {
		out_fdc(fdc->sc_bc, fdc->sc_ioh, NE7CMD_SENSEI);
		(void) fdcresult(fdc);
		n = 2;
	}

	printf("%s: %s", dv->dv_xname, s);

	switch (n) {
	case 0:
		printf("\n");
		break;
	case 2:
		printf(" (st0 %b cyl %d)\n",
		    fdc->sc_status[0], NE7_ST0BITS,
		    fdc->sc_status[1]);
		break;
	case 7:
		printf(" (st0 %b st1 %b st2 %b cyl %d head %d sec %d)\n",
		    fdc->sc_status[0], NE7_ST0BITS,
		    fdc->sc_status[1], NE7_ST1BITS,
		    fdc->sc_status[2], NE7_ST2BITS,
		    fdc->sc_status[3], fdc->sc_status[4], fdc->sc_status[5]);
		break;
#ifdef DIAGNOSTIC
	default:
		printf("\nfdcstatus: weird size");
		break;
#endif
	}
}

void
fdcpseudointr(arg)
	void *arg;
{
	int s;

	/* Just ensure it has the right spl. */
	s = splbio();
	(void) fdcintr(arg);
	splx(s);
}

int
fdcintr(arg)
	void *arg;
{
	struct fdc_softc *fdc = arg;

#if NFD > 0
	extern int fdintr __P((struct fdc_softc *));

	/* Will switch on device type, shortly. */
	return (fdintr(fdc));
#else
	printf("fdcintr: got interrupt, but no devices!\n");
#endif
}
