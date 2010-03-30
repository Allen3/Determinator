#if LAB >= 5

#include <inc/x86.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/mem.h>
#include <kern/net.h>

#include <dev/pic.h>
#include <dev/ioapic.h>
#include <dev/pci.h>
#include <dev/e100.h>


uint8_t e100_irq;

#define E100_TX_SLOTS			64
#define E100_RX_SLOTS			64

#define E100_NULL			0xffffffff
#define E100_SIZE_MASK			0x3fff	// mask out status/control bits

#define	E100_CSR_SCB_STATACK		0x01	// scb_statack (1 byte)
#define	E100_CSR_SCB_COMMAND		0x02	// scb_command (1 byte)
#define	E100_CSR_SCB_GENERAL		0x04	// scb_general (4 bytes)
#define	E100_CSR_PORT			0x08	// port (4 bytes)

#define E100_PORT_SOFTWARE_RESET	0

#define E100_SCB_COMMAND_CU_START	0x10
#define E100_SCB_COMMAND_CU_RESUME	0x20

#define E100_SCB_COMMAND_RU_START	1
#define E100_SCB_COMMAND_RU_RESUME	2

#define E100_SCB_STATACK_RNR		0x10
#define E100_SCB_STATACK_CNA		0x20
#define E100_SCB_STATACK_FR		0x40
#define E100_SCB_STATACK_CXTNO		0x80

// commands
#define E100_CB_COMMAND_XMIT		0x4

// command flags
#define E100_CB_COMMAND_SF		0x0008	// simple/flexible mode
#define E100_CB_COMMAND_I		0x2000	// interrupt on completion
#define E100_CB_COMMAND_S		0x4000	// suspend on completion

#define E100_CB_STATUS_C		0x8000

#define E100_RFA_STATUS_OK		0x2000	// packet received okay
#define E100_RFA_STATUS_C		0x8000	// packet reception complete

#define E100_RFA_CONTROL_SF		0x0008	// simple/flexible memory mode
#define E100_RFA_CONTROL_S		0x4000	// suspend after reception


struct e100_cb_tx {
	volatile uint16_t cb_status;
	volatile uint16_t cb_command;
	volatile uint32_t link_addr;
	volatile uint32_t tbd_array_addr;
	volatile uint16_t byte_count;
	volatile uint8_t tx_threshold;
	volatile uint8_t tbd_number;
};

// Transmit Buffer Descriptor (TBD)
struct e100_tbd {
	volatile uint32_t tb_addr;
	volatile uint16_t tb_size;
	volatile uint16_t tb_pad;
};

// Receive Frame Descriptor (RFD)
struct e100_rfd {
	// Fields common to all i8255x chips.
	volatile uint16_t status;
	volatile uint16_t control;
	volatile uint32_t link_addr;
	volatile uint32_t rbd_addr;
	volatile uint16_t actual;
	volatile uint16_t size;
};

// Receive Buffer Descriptor (RBD)
struct e100_rbd {
	volatile uint16_t rbd_count;
	volatile uint16_t rbd_pad0;
	volatile uint32_t rbd_link;
	volatile uint32_t rbd_buffer;
	volatile uint16_t rbd_size;
	volatile uint16_t rbd_pad1;
};

struct e100_tx_slot {
	struct e100_cb_tx tcb;	// Transmit command block
	char buf[NET_MAXPKT];	// Buffer - must immediately follow TCB
};

struct e100_rx_slot {
	struct e100_rfd rfd;	// Receive frame descriptor
	char buf[NET_MAXPKT];	// Buffer - must immediately follow RFD
};

static struct {
	uint32_t iobase;

	struct e100_tx_slot tx[E100_TX_SLOTS];
	int tx_head;
	int tx_tail;
	char tx_idle;

	struct e100_rx_slot rx[E100_RX_SLOTS];
	int rx_tail;
	char rx_idle;
} the_e100;


static void udelay(unsigned int u)
{
	unsigned int i;
	for (i = 0; i < u; i++)
		inb(0x84);
}

static void
e100_scb_wait(void)
{
	int i;

	for (i = 0; i < 100000; i++)
		if (inb(the_e100.iobase + E100_CSR_SCB_COMMAND) == 0)
			return;
	
	cprintf("e100_scb_wait: timeout\n");
}

static void
e100_scb_cmd(uint8_t cmd)
{
	outb(the_e100.iobase + E100_CSR_SCB_COMMAND, cmd);
}

static void e100_tx_start(void)
{
	int i = the_e100.tx_tail % E100_TX_SLOTS;

	if (the_e100.tx_tail == the_e100.tx_head)
		panic("oops, no TCBs");

	if (the_e100.tx_idle) {
		e100_scb_wait();
		outl(the_e100.iobase + E100_CSR_SCB_GENERAL, 
		     mem_phys(&the_e100.tx[i].tcb));
		e100_scb_cmd(E100_SCB_COMMAND_CU_START);
		the_e100.tx_idle = 0;
	} else {
		e100_scb_wait();
		e100_scb_cmd(E100_SCB_COMMAND_CU_RESUME);
	}
}

int e100_tx(void *hdr, int hlen, void *body, int blen)
{
	assert(hlen + blen <= NET_MAXPKT);
	int i;

	if (the_e100.tx_head - the_e100.tx_tail == E100_TX_SLOTS) {
		warn("e100_tx: no transmit buffers");
		return 0;
	}

	i = the_e100.tx_head % E100_TX_SLOTS;

	// Copy the packet header and body into the transmit buffer
	memcpy(the_e100.tx[i].buf, hdr, hlen);
	memcpy(the_e100.tx[i].buf+hlen, body, blen);

	// Set up the transmit command block
	the_e100.tx[i].tcb.tbd_number = hlen + blen;
	the_e100.tx[i].tcb.cb_status = 0;
	the_e100.tx[i].tcb.cb_command = E100_CB_COMMAND_XMIT |
		E100_CB_COMMAND_I | E100_CB_COMMAND_S;
	the_e100.tx_head++;

	e100_tx_start();
	return 1;
}

static void e100_rx_start(void)
{
	int i = the_e100.rx_tail % E100_RX_SLOTS;

	if (the_e100.rx_idle) {
		e100_scb_wait();
		outl(the_e100.iobase + E100_CSR_SCB_GENERAL, 
		     mem_phys(&the_e100.rx[i].rfd));
		e100_scb_cmd(E100_SCB_COMMAND_RU_START);
		the_e100.rx_idle = 0;
	} else {
		e100_scb_wait();
		e100_scb_cmd(E100_SCB_COMMAND_RU_RESUME);
	}
}

static void e100_intr_tx(void)
{
	int i;

	// Bump tx_tail past all transmit commands that have completed
	for (; the_e100.tx_head != the_e100.tx_tail; the_e100.tx_tail++) {
		i = the_e100.tx_tail % E100_TX_SLOTS;
		if (!(the_e100.tx[i].tcb.cb_status & E100_CB_STATUS_C))
			break;
	}
}

static void e100_intr_rx(void)
{
	int *count;
	int i;

	for (; ; the_e100.rx_tail++) {
		i = the_e100.rx_tail % E100_RX_SLOTS;

		if (!(the_e100.rx[i].rfd.status & E100_RFA_STATUS_C))
			break;	// We've processed all received packets

		// Dispatch the received packet to our network stack.
		if (the_e100.rx[i].rfd.status & E100_RFA_STATUS_OK) {
			int len = the_e100.rx[i].rfd.actual & E100_SIZE_MASK;
			net_rx(the_e100.rx[i].buf, len);
		} else
			warn("e100: packet receive error: %x",
				the_e100.rx[i].rfd.status);

		// Get this receive buffer ready to be filled again
		the_e100.rx[i].rfd.status = 0;
		the_e100.rx[i].rfd.actual = 0;
	}
}

void e100_intr(void)
{
	int r;
	
	r = inb(the_e100.iobase + E100_CSR_SCB_STATACK);
	outb(the_e100.iobase + E100_CSR_SCB_STATACK, r);
	
	if (r & (E100_SCB_STATACK_CXTNO | E100_SCB_STATACK_CNA)) {
		r &= ~(E100_SCB_STATACK_CXTNO | E100_SCB_STATACK_CNA);
		e100_intr_tx();
	}

	if (r & E100_SCB_STATACK_FR) {
		r &= ~E100_SCB_STATACK_FR;
		e100_intr_rx();
	}

	if (r & E100_SCB_STATACK_RNR) {
		r &= ~E100_SCB_STATACK_RNR;
		the_e100.rx_idle = 1;
		e100_rx_start();
		cprintf("e100_intr: RNR interrupt, no RX bufs?\n");
	}

	if (r)
		cprintf("e100_intr: unhandled STAT/ACK %x\n", r);
}

int e100_attach(struct pci_func *pcif)
{
	int i, next;

	pci_func_enable(pcif);

	e100_irq = pcif->irq_line;
	the_e100.iobase = pcif->reg_base[1];
	the_e100.tx_idle = 1;
	the_e100.rx_idle = 1;

	// Reset the card
	outl(the_e100.iobase + E100_CSR_PORT, E100_PORT_SOFTWARE_RESET);
	udelay(10);

	// Setup TX DMA ring for CU
	for (i = 0; i < E100_TX_SLOTS; i++) {
		next = (i + 1) % E100_TX_SLOTS;
		memset(&the_e100.tx[i], 0, sizeof(the_e100.tx[i]));
		the_e100.tx[i].tcb.link_addr = mem_phys(&the_e100.tx[next].tcb);
		the_e100.tx[i].tcb.tbd_array_addr = ~0;
		the_e100.tx[i].tcb.tbd_number = 1;
		the_e100.tx[i].tcb.tx_threshold = 4;
	}

	// Setup RX DMA ring for RU
	for (i = 0; i < E100_RX_SLOTS; i++) {
		next = (i + 1) % E100_RX_SLOTS;
		memset(&the_e100.rx[i], 0, sizeof(the_e100.rx[i]));
		the_e100.rx[i].rfd.status = 0;
		the_e100.rx[i].rfd.control = E100_RFA_CONTROL_S;
		the_e100.rx[i].rfd.link_addr = mem_phys(&the_e100.rx[next].rfd);
		the_e100.rx[i].rfd.size = NET_MAXPKT;
	}

	// Enable network card interrupts
	pic_enable(e100_irq);
	ioapic_enable(e100_irq, 0);

	return 1;
}

#endif  // LAB >= 5