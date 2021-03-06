/*
 *  DiskLoader.cpp
 */

#include "Platform.h"

extern "C"
void entrypoint(void) __attribute__ ((naked)) __attribute__ ((section (".vectors")));
void entrypoint(void)
{
	asm volatile (
				  "eor	r1,		r1\n"	// Zero register
				  "out	0x3F,	r1\n"	// SREG
				  "ldi	r28,	0xFF\n" // Y-register
				  "ldi	r29,	0x0A\n"	// Y-register
				  "out	0x3E,	r29\n"	// SPH
				  "out	0x3D,	r28\n"	// SPL
				  "rjmp	main"			// Stack is all set up, start the main code
				  ::);
}

u8 _flashbuf[128];
u8 _inSync;
u8 _ok;
u16 do_reset = 0;
volatile u16 _timeout = 0;

void Program(u8 ep, u16 page, u8 count)
{
	u8 write = page < 28*1024;		// Don't write over bootloader please
	if (write)
		boot_page_erase(page);
	
	USB_Recv_block(ep,_flashbuf,count);	// Read while page is erasing
	
	if (!write)
		return;
	
	boot_spm_busy_wait();			// Wait until the memory is erased.
	
	count >>= 1;
	u16* p = (u16*)page;
	u16* b = (u16*)_flashbuf;
	for (u8 i = 0; i < count; i++)
		boot_page_fill(p++, b[i]);
	
    boot_page_write(page);
    boot_spm_busy_wait();
    boot_rww_enable ();
}

u16 _pulse;
void LEDPulse() 
{
	_pulse += 1;
	u8 p = _pulse >> 9;
	if (p > 63)
		p = 127-p;
	p += p;
	if (((u8)_pulse) > p)
		L_LED_OFF();
	else
		L_LED_ON();
}

#define HW_VER	 0x02
#define SW_MAJOR 0x01
#define SW_MINOR 0x10

#define STK_OK              0x10
#define STK_INSYNC          0x14  // ' '
#define CRC_EOP             0x20  // 'SPACE'
#define STK_GET_SYNC        0x30  // '0'

#define STK_GET_PARAMETER   0x41  // 'A'
#define STK_SET_DEVICE      0x42  // 'B'
#define STK_SET_DEVICE_EXT  0x45  // 'E'
#define STK_LOAD_ADDRESS    0x55  // 'U'
#define STK_UNIVERSAL       0x56  // 'V'
#define STK_PROG_PAGE       0x64  // 'd'
#define STK_READ_PAGE       0x74  // 't'
#define STK_READ_SIGN       0x75  // 'u'

extern const u8 _readSize[] PROGMEM;
const u8 _readSize[] = 
{
	STK_GET_PARAMETER,	1,
	STK_SET_DEVICE,		20,
	STK_SET_DEVICE_EXT,	5,
	STK_UNIVERSAL,		4,
	STK_LOAD_ADDRESS,	2,
	STK_PROG_PAGE,		3,
	STK_READ_PAGE,		3,
	0,0
};

extern const u8 _consts[] PROGMEM;
const u8 _consts[] = 
{
	SIGNATURE_0,
	SIGNATURE_1,
	SIGNATURE_2,
	HW_VER,		// Hardware version
	SW_MAJOR,	// Software major version
	SW_MINOR,	// Software minor version
	0x03,		// Unknown but seems to be required by avr studio 3.56
	0x00,		// 
};

void start_sketch()
{
	if (pgm_read_word(0) == -1)		// no sketch has been uploaded yet - stay in bootloader
		return;
	
	L_LED_OFF();
	RX_LED_OFF();
	TX_LED_OFF();
	/* move interrupts to application section:
	 * uses inline assembly because the procedure must be completed in four cycles.
	 */
	cli();	// disable interrupts first
	asm volatile (
				  "ldi r16,	  0x01\n"		// (1<<IVCE)	/* Enable change of interrupt vectors */
				  "out 0x35,  r16\n"		// MCUCR
				  "ldi r16,	  0x00\n"		// (1<<IVSEL)	/* Move interrupts to application flash section */
				  "out 0x35,  r16\n"		// MCUCR				  
				  );							
	UDCON = 1;		// Detatch USB
	UDIEN = 0;
	asm volatile (	// Reset vector to run firmware
				  "clr r30\n"
				  "clr r31\n"
				  "ijmp\n"
				  ::);
}

int main(void) __attribute__ ((naked));
int main() 
{		
	wdt_disable();
	BOARD_INIT();
	/* move interrupts to boot section:
	 * uses inline assembly because the procedure must be completed in four cycles.
	 * seems to fail if called before disabling WDT and calling BOARD_INIT()
	 */
	asm volatile (
				  "ldi r16,	  0x01\n"		// (1<<IVCE)	/* Enable change of interrupt vectors */
				  "out 0x35,  r16\n"		// MCUCR
				  "ldi r16,	  0x02\n"		// (1<<IVSEL)	/* Move interrupts to boot flash section */
				  "out 0x35,  r16\n"		// MCUCR				  
				  );	
	TX_LED_OFF();
	RX_LED_OFF();
	L_LED_OFF();
	USB.attach();	
	sei();
	
	_inSync = STK_INSYNC;
	_ok = STK_OK;
	
	for (;;) 
	{
		u8* packet = _flashbuf;
		u16 address = 0;
		for (;;)
		{		
			if (USB_Available(CDC_RX)) 
			{
				u8 cmd = USB_Recv(CDC_RX);
				
				// Read packet contents
				u8 len;
				const u8* rs = _readSize;
				for (;;) 
				{
					u8 c = pgm_read_byte(rs++);
					len = pgm_read_byte(rs++);
					if (c == cmd || c == 0)
						break;
				}
				_timeout = 0;
				// Read params
				USB_Recv_block(CDC_RX, packet, len);
				
				// Send a response
				u8 send = 0;
				const u8* pgm = _consts+7;
				if (STK_GET_PARAMETER == cmd)
				{
					u8 i = packet[0] - 0x80;
					if (i > 2)
						i = (i==0x18) ? 3 : 4;	
					pgm = _consts + i + 3;
					send = 1;
				} 
				else if (STK_UNIVERSAL == cmd) 
				{
					if (packet[0] == 0x30)
						pgm = _consts + packet[2];	
					send = 1;
				}
				else if (STK_READ_SIGN == cmd)
				{ 
					pgm = _consts;
					send = 3;
				}
				else if (STK_LOAD_ADDRESS == cmd) 
				{
					address = *((u16*)packet);	// word address
					address += address;
				}
				else if (STK_PROG_PAGE == cmd)
				{
					Program(CDC_RX, address, packet[1]);
				}
				else if (STK_READ_PAGE == cmd)
				{
					send = packet[1];
					pgm = (const u8*)address;
					address += send;
				}
				
				u16 countdown = 5000;
				while (countdown-- > 10 && !USB_Available(CDC_RX)) 
					;
				int x = USB_Recv(CDC_RX);
				if (x != -1 && x != ' ')
					break;
				
				USB_Send(CDC_TX, &_inSync, 1);
				
				if (send) 
					USB_Send(CDC_TX|TRANSFER_PGM, pgm, send);
				
				// Send ok
				USB_Send(CDC_TX|TRANSFER_RELEASE, &_ok, 1);
				
				if ('Q' == cmd) 
				{
					_delay_ms(100);
					start_sketch();
				}
			}
			else 
			{
				LEDPulse();
				_delay_us(100);
				if (_timeout-- == 1)
					start_sketch();
			}
		}
	}
}
