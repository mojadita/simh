/*

   Copyright (c) 2015-2016, John Forecast

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   JOHN FORECAST BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of John Forecast shall not
   be used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from John Forecast.

*/

/* cdc1700_iofw.c:      CDC1700 I/O framework
 */
#include "cdc1700_defs.h"

extern char INTprefix[];

extern void buildIOtable(void);
extern void buildDCtables(void);
extern void RaiseExternalInterrupt(DEVICE *);
extern void rebuildPending(void);

extern uint16 Areg, Qreg, IOAreg, IOQreg;
extern DEVICE *sim_devices[];
extern DEVICE *IOdev[];

extern IO_DEVICE **CMap[];

t_bool IOFWinitialized = FALSE;

/*
 * This I/O framework provides an implementation of a generic device. The
 * framework provides for up to 8 read and 8 write device registers. The
 * read device registers may be stored and read directly from the framework
 * or may cause an entry to the device-specific portion of the device
 * driver. The framework may be setup to dynamically reject I/O requests
 * by setting the appropriate bit in iod_rejmapR/iod_rejmapW. Note that
 * access to the status/function register (register 1) cannot be rejected by
 * the framework and must be implemented by the device-specific code. This
 * allows a device to go "offline" while it it processing requests. Each I/O
 * device using the framework uses an IO_DEVICE structure. Each IO_DEVICE
 * structure may be used by up to 2 DEVICEs (e.g. TTI and TTO).
 *
 * The framework provides support for 3 classes of interrupts:
 *
 *  1. One or more of the standard 3 interrupts (DATA, EOP and ALARM)
 *
 *     The framework handles this class by default. Most devices fall into
 *     this class.
 *
 *  2. As 1 above but one or more additional interrupts generated by other
 *     status bits.
 *
 *     The IO_DEVICE structure must include the iod_intr entry to handle
 *     the additional interrupt(s). The CD and DP device drivers use this
 *     mechanism for the "Ready and not busy" interrupt.
 *
 *  3. Completely non-standard interrupts.
 *
 *     Most of the framework is not used in this case. The IO_DEVICE
 *     structure must include the iod_raised entry to handle all of it's
 *     interrupts. The RTC device driver uses this mechanism.
 *
 *
 * The following fields are present in the IO_DEVICE structure:
 *
 *      char            *iod_name;      - Generic device name override
 *      char            *iod_model;     - Device model name
 *      enum IOdevtype  iod_type;       - Device type
 *                                        when driver supports multiple
 *                                        device types
 *      uint8           iod_equip;      - Equipment number/interrupt
 *      uint8           iod_station;    - Station number
 *      uint16          iod_interrupt;  - Interrupt mask bit
 *      uint16          iod_dcbase;     - Base address of DC (or zero)
 *      DEVICE          *iod_indev;     - Pointer to input device
 *      DEVICE          *iod_outdev;    - Pointer to output device
 *      UNIT            *iod_unit;      - Currently selected unit
 *      t_bool          (*iod_reject)(IO_DEVICE *, t_bool, uint8);
 *                                      - Check if should reject I/O
 *      enum IOstatus   (*iod_IOread)(IO_DEVICE *, uint8);
 *      enum IOstatus   (*iod_IOwrite)(IO_DEVICE *, uint8);
 *                                      - Device read/write routines
 *      enum IOstatus   (*iod_BDCread)(struct io_device *, uint16 *, uint8);
 *      enum IOstatus   (*iod_BDCwrite)(struct io_device *, uint16 *, uint8);
 *                                      - Device read/write routines entered
 *                                        from 1706 buffered data channel
 *      void            (*iod_state)(char *, DEVICE *, IO_DEVICE *);
 *                                      - Dump device state for debug
 *      t_bool          (*iod_intr)(IO_DEVICE *);
 *                                      - Check for non-standard interrupts
 *      uint16          (*iod_raised)(DEVICE *);
 *                                      - For completely non-standard
 *                                        interrupt handling
 *      void            (*iod_clear)(DEVICE *);
 *                                      - Perform clear controller operation
 *      uint16          iod_ienable;    - Device interrupt enables
 *      uint16          iod_oldienable; - Previous iod_ienable
 *      uint16          iod_imask;      - Valid device interrupts
 *      uint16          iod_dmask;      - Valid director command bits
 *      uint16          iod_smask;      - Valid status bits
 *      uint16          iod_cmask;      - Status bits to clear on
 *                                              "clear interrupts"
 *      uint16          iod_rmask;      - Register mask (vs. station addr)
 *      uint8           iod_regs;       - # of device registers
 *      uint8           iod_validmask;  - Bitmap of valid registers
 *      uint8           iod_readmap;    - Bitmap of read registers
 *      uint8           iod_rejmapR;    - Bitmaps of register R/W
 *      uint8           iod_rejmapW;            access to be rejected
 *      uint8           iod_flags;      - Device flags
 * #define STATUS_ZERO  0x01            - Status register read returns 0
 * #define DEVICE_DC    0x02            - Device is buffered data channel
 * #define AQ_ONLY      0x04            - Device only works on the AQ channel
 *      uint8           iod_dc;         - Buffered Data Channel (0 => None)
 *      uint16          iod_readR[8];   - Device read registers
 *      uint16          iod_writeR[8];  - Device write registers
 *      uint16          iod_prevR[8];   - Previous device write registers
 *      uint16          iod_forced;     - Status bits forced to 1
 *      t_uint64        iod_event;      - Available for timestamping
 *      uint16          iod_private;    - Device-specific use
 *      void            *iod_private2;  - Device-specific use
 *      uint16          iod_private3;   - Device-specific use
 *      t_bool          iod_private4;   - Device-specific use
 *      void            *iod_private5;  - Device-specific use
 *      uint16          iod_private6;   - Device-specific use
 *      uint16          iod_private7;   - Device-specific use
 *      uint16          iod_private8;   - Device-specific use
 *      uint8           iod_private9;   - Device-specific use
 *      t_bool          iod_private10;  - Device-specific use
 *
 * The macro CHANGED(iod, n) will return what bits have been changed in write
 * register 'n' just after it has been written.
 *
 * The macro ICHANGED(iod) will return what interrupt enable bits have been
 * changed just after a director function has been issued.
 */

/*
 * Once-only initialization routine
 */
void fw_init(void)
{
  DEVICE *dptr;
  int i = 0;

  /*
   * Scan the device table and fill in the DEVICE back pointer(s)
   */
  while ((dptr = sim_devices[i++]) != NULL) {
    IO_DEVICE *iod = (IO_DEVICE *)dptr->ctxt;
    uint8 interrupt = iod->iod_equip;

    if ((dptr->flags & DEV_INDEV) != 0)
      iod->iod_indev = dptr;
    if ((dptr->flags & DEV_OUTDEV) != 0)
      iod->iod_outdev = dptr;

    /*
     * Fill in the interrupt mask bit.
     */
    iod->iod_interrupt = 1 << interrupt;
  }

  /*
   * Build the I/O device and buffered data channel tables.
   */
  buildIOtable();
  buildDCtables();

  IOFWinitialized = TRUE;
}

/*
 * Perform I/O operation - called directly from the IN/OUT instruction
 * processing.
 */
enum IOstatus fw_doIO(DEVICE *dptr, t_bool output)
{
  IO_DEVICE *iod = (IO_DEVICE *)dptr->ctxt;
  uint8 rej = (output ? iod->iod_rejmapW : iod->iod_rejmapR) & ~MASK_REGISTER1;
  uint8 reg;

  if ((iod->iod_flags & DEVICE_DC) != 0)
    reg = ((IOQreg & IO_W) - iod->iod_dcbase) >> 11;
  else reg = IOQreg & iod->iod_rmask;

  /*
   * Check for valid device address
   */
  if (reg >= iod->iod_regs)
    return IO_REJECT;

  /*
   * Check if we should reject this request
   */
  if ((rej & (1 << reg)) != 0)
    return IO_REJECT;

  /*
   * Check if we should reject this request
   */
  if (iod->iod_reject != NULL)
    if ((*iod->iod_reject)(iod, output, reg))
      return IO_REJECT;

  if (output) {
    iod->iod_prevR[reg] = iod->iod_writeR[reg];
    iod->iod_writeR[reg] = Areg;
    return (*iod->iod_IOwrite)(iod, reg);
  }

  if ((iod->iod_readmap & (1 << reg)) != 0) {
    Areg = iod->iod_readR[reg];
    return IO_REPLY;
  }

  return (*iod->iod_IOread)(iod, reg);
}

/*
 * Perform I/O operation - called from the buffered data channel controller.
 */
enum IOstatus fw_doBDCIO(IO_DEVICE *iod, uint16 *data, t_bool output, uint8 reg)
{
  uint8 rej = (output ? iod->iod_rejmapW : iod->iod_rejmapR) & ~MASK_REGISTER1;
  DEVICE *dptr = iod->iod_indev;
  enum IOstatus status;
  
  IOAreg = *data;

  /*
   * Check for valid device address
   */
  if (reg >= iod->iod_regs)
    return IO_REJECT;

  /*
   * Check if we should reject this request
   */
  if ((rej & (1 << reg)) != 0)
    return IO_REJECT;

  /*
   * Check if we should reject this request
   */
  if (iod->iod_reject != NULL)
    if ((*iod->iod_reject)(iod, output, reg))
      return IO_REJECT;

  if ((dptr->dctrl & DBG_DSTATE) != 0)
    if (iod->iod_state != NULL)
      (*iod->iod_state)("before BDC I/O", dptr, iod);

  if (output) {
    iod->iod_prevR[reg] = iod->iod_writeR[reg];
    iod->iod_writeR[reg] = *data;
    status = (*iod->iod_BDCwrite)(iod, data, reg);
  } else {
    if ((iod->iod_readmap & (1 << reg)) != 0) {
      *data = iod->iod_readR[reg];
      
      if ((dptr->dctrl & DBG_DSTATE) != 0)
        if (iod->iod_state != NULL)
          (*iod->iod_state)("after cached BDC I/O", dptr, iod);

      return IO_REPLY;
    }

    status = (*iod->iod_BDCread)(iod, data, reg);
  }

  if ((dptr->dctrl & DBG_DSTATE) != 0)
    if (iod->iod_state != NULL)
      (*iod->iod_state)("after BDC I/O", dptr, iod);

  return status;
}

/*
 * Devices may support multiple interrupts (DATA, EOP and ALARM are standard)
 * but there is only 1 active interrupt flag (IO_ST_INT). This means that we
 * must make sure that the active interrupt flag is set whenever one or more
 * interrupt source is active and the interrupt(s) have been enabled.
 * Interrupts are typically generated when a status flag is raised but we also
 * need to handle removing an interrupt souce when a flag is dropped.
 *
 * In addition, some devices have non-standard interrupts and we need to
 * provide a callback to a device-specific routine to check for such
 * interrupts.
 */
void fw_IOintr(t_bool other, DEVICE *dev, IO_DEVICE *iod, uint16 set, uint16 clr, uint16 mask, const char *why)
{
  /*
   * Set/clear the requested status bits.
   */
  DEVSTATUS(iod) &= ~(clr | IO_ST_INT);
  DEVSTATUS(iod) |= set | iod->iod_forced;
  DEVSTATUS(iod) &= (mask & iod->iod_smask);

  rebuildPending();

  /*
   * Check for any interrupts enabled.
   */
  if (ISENABLED(iod, iod->iod_imask)) {
    t_bool intr = FALSE;

    /*
     * Check standard interrupts
     */
    if ((ISENABLED(iod, IO_DIR_ALARM) &&
         (((DEVSTATUS(iod) & IO_ST_ALARM) != 0))) ||
        (ISENABLED(iod, IO_DIR_EOP) &&
         (((DEVSTATUS(iod) & IO_ST_EOP) != 0))) ||
        (ISENABLED(iod, IO_DIR_DATA) &&
         (((DEVSTATUS(iod) & IO_ST_DATA) != 0))))
      intr = TRUE;

    /*
     * If the device has non-standard interrupts, call a device-specific
     * routine to determine if IO_ST_INT should be set.
     */
    if (other)
      if (iod->iod_intr != NULL)
        if (iod->iod_intr(iod))
          intr = TRUE;

    if (intr) {
      DEVSTATUS(iod) |= IO_ST_INT;

      if (why != NULL) {
        if ((dev->dctrl & DBG_DINTR) != 0)
          fprintf(DBGOUT, "%s%s Interrupt - %s, Ena: %04X, Sta: %04X\r\n",
                  INTprefix, dev->name, why, ENABLED(iod), DEVSTATUS(iod));
        RaiseExternalInterrupt(dev);
      } else rebuildPending();
    }
  }
}

/*
 * The following routines are only valid if the framework handles the device
 * status register and the function register (register 1) handles interrupt
 * enable at end of processing.
 */
/*
 * 1. Devices which use IO_ST_DATA to signal end of processing.
 */
void fw_IOunderwayData(IO_DEVICE *iod, uint16 clr)
{
  DEVSTATUS(iod) &= ~(clr | IO_ST_READY | IO_ST_DATA);
  DEVSTATUS(iod) |= IO_ST_BUSY;
  DEVSTATUS(iod) |= iod->iod_forced;
  DEVSTATUS(iod) &= iod->iod_smask;
}

void fw_IOcompleteData(t_bool other, DEVICE *dev, IO_DEVICE *iod, uint16 mask, const char *why)
{
  fw_IOintr(other, dev, iod, IO_ST_READY | IO_ST_DATA, IO_ST_BUSY, mask, why);
}

/*
 * 2. Devices which use IO_ST_EOP to signal end of processing.
 */
void fw_IOunderwayEOP(IO_DEVICE *iod, uint16 clr)
{
  DEVSTATUS(iod) &= ~(clr | IO_ST_READY | IO_ST_EOP);
  DEVSTATUS(iod) |= IO_ST_BUSY;
  DEVSTATUS(iod) |= iod->iod_forced;
  DEVSTATUS(iod) &= iod->iod_smask;
}

void fw_IOcompleteEOP(t_bool other, DEVICE *dev, IO_DEVICE *iod, uint16 mask, const char *why)
{
  fw_IOintr(other, dev, iod, IO_ST_READY | IO_ST_EOP, IO_ST_BUSY, mask, why);
}

/*
 * 3. Devices which use IO_ST_EOP to signal end of processing, but do not
 *    drop IO_ST_READY while I/O is in progress.
 */
void fw_IOunderwayEOP2(IO_DEVICE *iod, uint16 clr)
{
  DEVSTATUS(iod) &= ~(clr | IO_ST_EOP);
  DEVSTATUS(iod) |= IO_ST_BUSY;
  DEVSTATUS(iod) |= iod->iod_forced;
  DEVSTATUS(iod) &= iod->iod_smask;
}

void fw_IOcompleteEOP2(t_bool other, DEVICE *dev, IO_DEVICE *iod, uint16 mask, const char *why)
{
  fw_IOintr(other, dev, iod, IO_ST_EOP, IO_ST_BUSY, mask, why);
}

void fw_IOalarm(t_bool other, DEVICE *dev, IO_DEVICE *iod, const char *why)
{
  fw_IOintr(other, dev, iod, IO_ST_ALARM, IO_ST_BUSY, 0xFFFF, why);
}

/*
 * The following routine manipulates "forced" status bits. This allows
 * certain status bits to remain set while the basic I/O framework assumes
 * that it will manipulate such bits, for example, IO_ST_BUSY and
 * IO_ST_READY for the Paper Tape Reader.
 */
void fw_setForced(IO_DEVICE *iod, uint16 mask)
{
  iod->iod_forced |= mask;
  DEVSTATUS(iod) |= (mask & iod->iod_smask);
}

void fw_clearForced(IO_DEVICE *iod, uint16 mask)
{
  iod->iod_forced &= ~mask;
  DEVSTATUS(iod) &= ~mask;
}

/*
 * Generic device reject check. If the device is not ready, reject all OUTs
 * unless it is to the director function register (register 1).
 */
t_bool fw_reject(IO_DEVICE *iod, t_bool output, uint8 reg)
{
  if (output && (reg != 1)) {
    return (DEVSTATUS(iod) & IO_ST_READY) == 0;
  }
  return FALSE;
}

/*
 * Generic dump routine for a simple device with a function and status
 * register.
 */
void fw_state(char *where, DEVICE *dev, IO_DEVICE *iod)
{
  fprintf(DBGOUT, "%s[%s %s state: Function: %04X, Status: %04x]\r\n",
          INTprefix, dev->name, where, iod->FUNCTION, DEVSTATUS(iod));
}

/*
 * Find a buffered data channel device which supports a specified I/O
 * address. Note that since none of the current devices which can make use
 * of a buffered data channel include a station address, we can just
 * perform a simple range check.
 */
IO_DEVICE *fw_findChanDevice(IO_DEVICE *iod, uint16 addr)
{
  DEVICE *dptr = iod->iod_indev;
  DEVICE *target = IOdev[(addr & IO_EQUIPMENT) >> 7];
  uint32 i;

  if (target != NULL) {
    for (i = 0; i < dptr->numunits; i++) {
      UNIT *uptr = &dptr->units[i];

      if (uptr->up8 == target->ctxt)
        return (IO_DEVICE *)target->ctxt;
    }
  }
  return NULL;
}
