/*
;    Project:       Open Vehicle Monitor System
;    Date:          16 October 2011
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011  Michael Stegen / Stegen Electronics
;    (C) 2011  Mark Webb-Johnson
;    (C) 2011  Sonny Chen @ EPRO/DX
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include <stdio.h>
#include <usart.h>
#include <string.h>
#include <stdlib.h>
#include <delays.h>
#include "ovms.h"
#include "net_sms.h"
#include "net_msg.h"
#include "utils.h"
#include "led.h"
#include "inputs.h"
#ifdef OVMS_DIAGMODULE
#include "diag.h"
#endif // #ifdef OVMS_DIAGMODULE
#ifdef OVMS_LOGGINGMODULE
#include "logging.h"
#endif // #ifdef OVMS_LOGGINGMODULE

// NET data
#pragma udata
unsigned char net_state = 0;                // The current state
unsigned char net_state_vchar = 0;          //   A per-state CHAR variable
unsigned int  net_state_vint = NET_GPRS_RETRIES; //   A per-state INT variable
unsigned char net_cops_tries = 0;           // A counter for COPS attempts
unsigned char net_timeout_goto = 0;         // State to auto-transition to, after timeout
unsigned int  net_timeout_ticks = 0;        // Number of seconds before timeout auto-transition
unsigned int  net_granular_tick = 0;        // An internal ticker used to generate 1min, 5min, etc, calls
unsigned int  net_watchdog = 0;             // Second count-down for network connectivity
unsigned int  net_timeout_rxdata = NET_RXDATA_TIMEOUT; // Second count-down for RX data timeout
char net_caller[NET_TEL_MAX] = {0};         // The telephone number of the caller

unsigned char net_buf_pos = 0;              // Current position (aka length) in the network buffer
unsigned char net_buf_mode = NET_BUF_CRLF;  // Mode of the buffer (CRLF, SMS or MSG)
unsigned char net_buf_todo = 0;             // Bytes outstanding on a reception
unsigned char net_buf_todotimeout = 0;      // Timeout for bytes outstanding

unsigned char net_fnbits = 0;               // Net functionality bits

#ifdef OVMS_SOCALERT
unsigned char net_socalert_sms = 0;         // SOC Alert (msg) 10min ticks remaining
unsigned char net_socalert_msg = 0;         // SOC Alert (sms) 10min ticks remaining
#endif //#ifdef OVMS_SOCALERT

#ifndef OVMS_NO_ERROR_NOTIFY
unsigned int  net_notify_errorcode = 0;     // An error code to be notified
unsigned long net_notify_errordata = 0;     // Ancilliary data
unsigned int  net_notify_lasterrorcode = 0; // Last error code to be notified
unsigned char net_notify_lastcount = 0;     // A counter used to clear error codes
#endif //OVMS_NO_ERROR_NOTIFY

unsigned int  net_notify = 0;               // Bitmap of notifications outstanding
unsigned char net_notify_suppresscount = 0; // To suppress STAT notifications (seconds)

#pragma udata NETBUF_SP
char net_scratchpad[NET_BUF_MAX];           // A general-purpose scratchpad
#pragma udata
#pragma udata NETBUF
char net_buf[NET_BUF_MAX];                  // The network buffer itself
#pragma udata

// ROM Constants

#if defined(OVMS_INTERNALGPS) && defined(OVMS_SIMCOM_SIM908)
// Using internal SIM908 GPS:
rom char NET_WAKEUP_GPSON[] = "AT+CGPSPWR=1\r";
rom char NET_WAKEUP_GPSOFF[] = "AT+CGPSPWR=0\r";
rom char NET_REQGPS[] = "AT+CGPSINF=2;+CGPSINF=64;+CGPSPWR=1\r";
rom char NET_INIT1_GPSON[] = "AT+CGPSRST=0;+CSMINS?\r";
#elif defined(OVMS_INTERNALGPS) && defined(OVMS_SIMCOM_SIM808)
// Using internal SIM808 GPS:
rom char NET_WAKEUP_GPSON[] = "AT+CGNSPWR=1\r";
rom char NET_WAKEUP_GPSOFF[] = "AT+CGNSPWR=0\r";
rom char NET_REQGPS[] = "AT+CGNSINF\r";
rom char NET_INIT1_GPSON[] = "AT+CSMINS?\r";
#else
// Using external GPS from car:
rom char NET_WAKEUP[] = "AT\r";
#endif

rom char NET_INIT1[] = "AT+CSMINS?\r";

#ifndef OVMS_NO_PHONEBOOKAP
rom char NET_INIT2[] = "AT+CCID;+CPBF=\"O-\";+CPIN?\r";
#else
rom char NET_INIT2[] = "AT+CCID;+CPIN?\r";
#endif

#ifdef OVMS_DIAGDATA
// using DIAG port for data output: enable modem echo
rom char NET_INIT3[] = "AT+IPR?;+CREG=1;+CLIP=1;+CMGF=1;+CNMI=2,2;+CSDH=1;+CMEE=2;+CIPSPRT=1;+CIPQSEND=1;+CLTS=1;E1\r";
#else
// default: disable modem echo
rom char NET_INIT3[] = "AT+IPR?;+CREG=1;+CLIP=1;+CMGF=1;+CNMI=2,2;+CSDH=1;+CMEE=2;+CIPSPRT=1;+CIPQSEND=1;+CLTS=1;E0\r";
#endif // OVMS_DIAGDATA

rom char NET_COPS[] = "AT+COPS=0,1;+COPS?\r";

rom char NET_HANGUP[] = "ATH\r";
rom char NET_CREG_CIPSTATUS[] = "AT+CREG?;+CIPSTATUS;+CCLK?;+CSQ\r";
rom char NET_CREG_STATUS[] = "AT+CREG?\r";
rom char NET_IPR_SET[] = "AT+IPR=9600\r"; // sets fixed baud rate for the modem

////////////////////////////////////////////////////////////////////////
// The Interrupt Service Routine is standard PIC code
//
// It takes the incoming async data and puts it in an internal buffer,
// and also outputs data queued for transmission.
//
void low_isr(void);

// serial interrupt taken as low priority interrupt
#pragma code uart_int_service = 0x18
void uart_int_service(void)
  {
  _asm goto low_isr _endasm
  }
#pragma code

// ISR optimization, see http://www.xargs.com/pic/c18-isr-optim.pdf
#pragma tmpdata low_isr_tmpdata
#pragma	interruptlow low_isr nosave=section(".tmpdata")
void low_isr(void)
  {
  // call of library module function, MUST
  UARTIntISR();
  led_isr();
  }
#pragma tmpdata


////////////////////////////////////////////////////////////////////////
// net_reset_async()
// Reset the async status following an overun or other such error
//
void net_reset_async(void)
  {
  vUARTIntStatus.UARTIntRxError = 0;
  }


////////////////////////////////////////////////////////////////////////
// net_assert_caller():
// check for valid (non empty) caller, fallback to PARAM_REGPHONE
//
char *net_assert_caller(char *caller)
  {
  if (!caller || !*caller)
    {
    strcpy(net_caller, par_get(PARAM_REGPHONE));
    return(net_caller);
    }
  return caller;
  }


////////////////////////////////////////////////////////////////////////
// net_poll()
// This function is an entry point from the main() program loop, and
// gives the NET framework an opportunity to poll for data.
//
// It polls the interrupt-handler async buffer and move characters into
// net_buf, depending on net_buf_mode. This internally handles normal
// line-by-line (CRLF) data, as well as SMS (+CMT) and MSG (+IPD) modes.
//
// This function is also the dispatcher for net_state_activity(),
// to pass the incoming data to the current state for handling.
//
void net_poll(void)
  {
  unsigned char x;

  CHECKPOINT(0x30)

  while(UARTIntGetChar(&x))
    {
    
    if (net_buf_mode==NET_BUF_CRLF)
      { // CRLF (either normal or SMS second line) mode
      
      // Special handling in FIRSTRUN and DIAGMODE (= human input):
      if ((net_state==NET_STATE_FIRSTRUN)||(net_state==NET_STATE_DIAGMODE))
        {
        if (x == 0x0a) // Skip 0x0a (LF)
          continue;
        else if (x == 0x0d) // Treat CR as LF
          x = 0x0a;
        else if (x == 0x08) // Backspace
          {
          if (net_buf_pos > 0)
            net_buf[--net_buf_pos] = 0;
          continue;
          }
        else if (x == 0x01 || x == 0x03) // Ctrl-A / Ctrl-C
          {
          net_buf_pos = 0;
          net_puts_rom("\r\n");
          continue;
          }
        } // NET_STATE_FIRSTRUN || NET_STATE_DIAGMODE
      
      // Skip 0x0d (CR)
      if (x == 0x0d) continue;
      
      // Add char to buffer:
      net_buf[net_buf_pos++] = x;
      if (net_buf_pos == NET_BUF_MAX) net_buf_pos--;
      
      // Switch to IP data mode (NET_BUF_IPD)?
      if ((x == ':')&&(net_buf_pos>=6)&&
          (net_buf[0]=='+')&&(net_buf[1]=='I')&&
          (net_buf[2]=='P')&&(net_buf[3]=='D')&&
          (!vUARTIntStatus.UARTIntRxOverFlow))
        {
        CHECKPOINT(0x31)
        // Syntax: +IPD,<length>:<msg>
        
        // Get message length:
        net_buf[net_buf_pos-1] = 0; // Change the ':' to an end
        net_buf_todo = atoi(net_buf+5); // Length of IP message
        
        net_buf_todotimeout = 60; // 60 seconds to receive the rest
        net_buf_pos = 0;
        net_buf_mode = NET_BUF_IPD;
        continue; // We have switched to IPD mode
        }
      
      // Newline?
      if (x == 0x0A)
        {
        CHECKPOINT(0x32)
        net_buf_pos--;
        net_buf[net_buf_pos] = 0; // mark end of string for string search functions.
        
        // Switch to SMS data mode (NET_BUF_SMS)?
        if ((net_buf_pos>=4)&&
            (net_buf[0]=='+')&&(net_buf[1]=='C')&&
            (net_buf[2]=='M')&&(net_buf[3]=='T')&&
            (!vUARTIntStatus.UARTIntRxOverFlow))
          {
          // Syntax: +CMT: "<caller>","","yy/mm/dd,HH:MM:SS+04",145,32,0,0,"gateway",145,<length>
          
          // Get caller phone number:
          x = 7;
          while ((net_buf[x] != '\"') && (x < net_buf_pos)) x++; // search for end of phone number
          net_buf[x] = '\0'; // mark end of string
          strncpy(net_caller,net_buf+7,NET_TEL_MAX);
          net_caller[NET_TEL_MAX-1] = '\0';
          
          // Get message length:
          x = net_buf_pos;
          while ((net_buf[x] != ',') && (x > 0)) x--; // Search for last comma seperator
          net_buf_todo = atoi(net_buf+x+1); // Length of SMS message
          
          net_buf_todotimeout = NET_BUF_TIMEOUT; // seconds to receive the rest
          net_buf_pos = 0;
          net_buf_mode = NET_BUF_SMS;
          continue; // We have switched to SMS mode
          }
        
        // Handle message:
        net_state_activity();
        
        // Reset buffer, stay in CRLF mode:
        net_buf_pos = 0;
        net_buf_mode = NET_BUF_CRLF;
        
        } // Newline
      
      } // (net_buf_mode==NET_BUF_CRLF)
    
    else if (net_buf_mode==NET_BUF_SMS)
      { // SMS data mode
      CHECKPOINT(0x33)
      
      // Add char to buffer:
      if ((x==0x0d)||(x==0x0a))
        net_buf[net_buf_pos++] = ' '; // \d, \r => space
      else
        net_buf[net_buf_pos++] = x;
      if (net_buf_pos == NET_BUF_MAX) net_buf_pos--;
      net_buf_todo--;

      // Message complete?
      if (net_buf_todo==0)
        {
        net_buf[net_buf_pos] = 0; // Zero-terminate
        
        // Handle message:
        net_state_activity();
        
        // Reset buffer, switch back to CRLF mode:
        net_buf_pos = 0;
        net_buf_mode = NET_BUF_CRLF;
        }
      } // (net_buf_mode==NET_BUF_SMS)
    
    else // (net_buf_mode==NET_BUF_IPD)
      { // IP data mode
      CHECKPOINT(0x34)
      
      // Add char to buffer:
      if (x != 0x0d) // Swallow CR
        {
        net_buf[net_buf_pos++] = x;
        if (net_buf_pos == NET_BUF_MAX) net_buf_pos--;
        }
      net_buf_todo--;
      
      // Newline = message protocol termination?
      if (x == 0x0A)
        {
        net_buf_pos--;
        net_buf[net_buf_pos] = 0; // mark end of string for string search functions.
        
        // Handle message:
        net_state_activity();
        
        // Reset buffer, stay in IPD mode:
        net_buf_pos = 0;
        }
      
      // IP message complete?
      if (net_buf_todo==0)
        {
        // Reset buffer, switch back to CRLF mode:
        net_buf_pos = 0;
        net_buf_mode = NET_BUF_CRLF;
        }
      
      } // (net_buf_mode==NET_BUF_IPD)
    
    } // while(UARTIntGetChar(&x))
  
    // Zero-terminate buffer at current write position for overrun security:
    net_buf[net_buf_pos] = 0;
  }


////////////////////////////////////////////////////////////////////////
// net_wait4modem()
// 
// Wait for the modem to be PROBABLY ready for the next command.
// Use if you need to issue a command without being able to retry later
// if the modem is not ready.
// Note: waits for RX silence, which can also result from the
// RX buffer being full or the modem needing more time to respond.
//
void net_wait4modem()
  {
  UINT8 c, len;
  
  // wait for TX flush:
  if (!vUARTIntStatus.UARTIntTxBufferEmpty)
    {
    while (!vUARTIntStatus.UARTIntTxBufferEmpty);
    // add 25 ms processing time:
    delay5(5);
    }
  
  // wait for 50 ms RX silence (may also mean RX buffer is full...)
  for (c=0, len=vUARTIntRxBufDataCnt; c<10; len=vUARTIntRxBufDataCnt)
    {
    delay5b();
    if (vUARTIntRxBufDataCnt != len)
      c = 0;
    else
      c++;
    }
  
  }


////////////////////////////////////////////////////////////////////////
// net_wait4prompt()
// 
// Wait for the IP/SMS send prompt "> " or "ER"(ROR)
// Returns TRUE if prompt has been received.
// Timeout: ~1 s
// Note: does not fetch characters from the UART RX buffer, so net_poll()
// can read errors afterwards.
//
BOOL net_wait4prompt()
  {
  char c1, c2;
  UINT timeout = 5000; // x 0.2 ms = ~1 s
  
  do
    {
    // Peek last 2 characters from RX buffer:
    c1 = vUARTIntRxBuffer[((RX_BUFFER_SIZE-2)+vUARTIntRxBufWrPtr)%RX_BUFFER_SIZE];
    c2 = vUARTIntRxBuffer[((RX_BUFFER_SIZE-1)+vUARTIntRxBufWrPtr)%RX_BUFFER_SIZE];
    
    if (c1=='>' && c2==' ')
      return TRUE; // Prompt detected
    else if (c1=='E' && c2=='R')
      return FALSE; // Error detected
    
    // wait 0.2 ms
    Delay1KTCYx(1);
    
    } while(--timeout);
    
    // account prompt timeout:
    if (net_timeout_rxdata > 1) // leave 1s for ticker
      --net_timeout_rxdata;
    
    return FALSE;
  }


////////////////////////////////////////////////////////////////////////
// net_puts_rom()
// Transmit zero-terminated character data from ROM to the async port.
// N.B. This may block if the transmit buffer is full.
//

// Macro to wait for TxBuffer before call to PutChar():
#define UART_WAIT_PUTC(c) \
  { \
  while (vUARTIntStatus.UARTIntTxBufferFull) ; \
  while (UARTIntPutChar(c)==0) ; \
  }

void net_puts_rom(const rom char *data)
  {
  
  if (net_msg_bufpos)
    {
    // NET SMS wrapper mode: (189 byte = max MP payload)
    for (;*data && (net_msg_bufpos < (net_buf+189));data++)
      *net_msg_bufpos++ = *data;
    }

#ifdef OVMS_DIAGMODULE
  // Help diag terminals with line breaks
  else if ( net_state == NET_STATE_DIAGMODE )
    {
    char lastdata = 0;

    while(1)
      {
      if ( *data == '\n' && lastdata != '\r' )
        UART_WAIT_PUTC('\r') // insert \r before \n if missing:
      else if( lastdata == '\r' && *data != '\n' )
        {
        UART_WAIT_PUTC('\n') // insert \n after \r if missing
        if (net_msg_sendpending)
          {
          UART_WAIT_PUTC('#')
          UART_WAIT_PUTC(' ')
          }
        }

      if ( !*data )
        break;

      // output char
      UART_WAIT_PUTC(*data)
      
      if (net_msg_sendpending && *data=='\n')
        {
        UART_WAIT_PUTC('#')
        UART_WAIT_PUTC(' ')
        }

      lastdata = *data++;
      }
    }
#endif // OVMS_DIAGMODULE

  else
    {
    // Send characters up to the null
    for (;*data;data++)
      UART_WAIT_PUTC(*data)
    }
  }

////////////////////////////////////////////////////////////////////////
// net_puts_ram()
// Transmit zero-terminated character data from RAM to the async port.
// N.B. This may block if the transmit buffer is full.
//
void net_puts_ram(const char *data)
  {

  if (net_msg_bufpos)
    {
    // NET SMS wrapper mode: (189 byte = max MP payload)
    for (;*data && (net_msg_bufpos < (net_buf+189));data++)
      *net_msg_bufpos++ = *data;
    }

#ifdef OVMS_DIAGMODULE
  // Help diag terminals with line breaks
  else if( net_state == NET_STATE_DIAGMODE )
    {
    char lastdata = 0;

    while(1)
      {
      if ( *data == '\n' && lastdata != '\r' )
        UART_WAIT_PUTC('\r') // insert \r before \n if missing
      else if( lastdata == '\r' && *data != '\n' )
        {
        UART_WAIT_PUTC('\n') // insert \n after \r if missing
        if (net_msg_sendpending)
          {
          UART_WAIT_PUTC('#')
          UART_WAIT_PUTC(' ')
          }
        }

      if ( !*data )
        break;

      // output char
      UART_WAIT_PUTC(*data)

      if (net_msg_sendpending && *data=='\n')
        {
        UART_WAIT_PUTC('#')
        UART_WAIT_PUTC(' ')
        }

      lastdata = *data++;
      }
    }
#endif // OVMS_DIAGMODULE

  else
    {
    // Send characters up to the null
    for (;*data;data++)
      UART_WAIT_PUTC(*data)
    }
  }

////////////////////////////////////////////////////////////////////////
// net_putc_ram()
// Transmit a single character from RAM to the async port.
// N.B. This may block if the transmit buffer is full.
void net_putc_ram(const char data)
  {
  if (net_msg_bufpos)
    {
    // NET SMS wrapper mode: (189 byte = max MP payload)
    if (net_msg_bufpos < (net_buf+189))
      *net_msg_bufpos++ = data;
    }
  else
    {
    // Send one character
    UART_WAIT_PUTC(data)
    }
  }

#ifndef OVMS_NO_ERROR_NOTIFY
////////////////////////////////////////////////////////////////////////
// net_req_notification_error()
// Request notification of an error
void net_req_notification_error(unsigned int errorcode, unsigned long errordata)
  {
  if (errorcode != 0)
    {
    // We have an error being set
    if ((errorcode != net_notify_lasterrorcode)&&
        ((sys_features[FEATURE_CARBITS]&FEATURE_CB_SVALERTS)==0))
      {
      // This is a new error, so set it and time it out after 60 seconds
      net_notify_errorcode = errorcode;
      net_notify_errordata = errordata;
      net_notify_lasterrorcode = errorcode;
      net_notify_lastcount = 60;
      }
    else
      {
      // Reset the timer for another 60 seconds
      net_notify_lastcount = 60;
      }
    }
  else
    {
    // Clear the error
    net_notify_errorcode = 0;
    net_notify_errordata = 0;
    net_notify_lasterrorcode = 0;
    net_notify_lastcount = 0;
    }
  }
#endif //OVMS_NO_ERROR_NOTIFY

////////////////////////////////////////////////////////////////////////
// net_req_notification()
// Request notification of one or more of the types specified
// in net.h NET_NOTIFY_*
//
void net_req_notification(unsigned int notify)
  {
  char *p;
  p = par_get(PARAM_NOTIFIES);
  if (strstrrampgm(p,(char const rom far*)"SMS") != NULL)
    {
    net_notify |= (notify<<8); // SMS notification flags are top 8 bits
    }
  if (strstrrampgm(p,(char const rom far*)"IP") != NULL)
    {
    net_notify |= notify;      // NET notification flags are bottom 8 bits
    }
  }


#ifndef OVMS_NO_PHONEBOOKAP
////////////////////////////////////////////////////////////////////////
// net_phonebook: SIM phonebook auto provisioning system
//
// Usage:
// Create phonebook entries like "O-8-MYVEHICLEID", "O-5-APN", etc.
// The O- prefix tells us this is OVMS, the next number is the parameter
// number to set, and the remainder is the value.
// This is very flexible, but the textual fields are limited in length
// (typically 16 characters or so).
// 
void net_phonebook(char *pb)
  {
  // We have a phonebook entry line from the SIM card / modem
  // Looking like: +CPBF: 1,"0",129,"O-X-Y"
  char *n,*p, *ep;

  n = firstarg(pb,'\"');
  if (n==NULL) return;
  n = nextarg(n);
  if (n==NULL) return;
  p = nextarg(n);
  if (p==NULL) return;
  p = nextarg(p);
  if (p==NULL) return;

  // At this point, *n is the second token (the number), and *p is
  // the fourth token (the parameter). Both are defined.
  if ((p[0]=='O')&&(p[1]=='-'))
    {
    n = firstarg(p+2,'-');
    if (n==NULL) return;
    p = nextarg(n);
    ep = par_get(atoi(n));
    if (p==NULL)
      {
      if (ep[0] != 0)
        par_set((unsigned char)atoi(n), (char*)"");
      }
    else
      {
      if (strcmp(ep,p) != 0)
        par_set((unsigned char)atoi(n), p);
      }
    }
  }
#endif //OVMS_NO_PHONEBOOKAP


////////////////////////////////////////////////////////////////////////
// net_state_enter(newstate)
// State Model: A new state has been entered.
// This should do any initialisation and other actions required on
// a per-state basis. It is called when the state is entered.
//
void net_state_enter(unsigned char newstate)
  {
  char *p;

// Enable for debugging (via serial port) of state transitions
//  p = stp_x(net_scratchpad, "\r\n# ST-ENTER: ", newstate);
//  p = stp_rom(p, "\r\n");
//  net_puts_ram(net_scratchpad);
//  delay100(1);
  
  net_state = newstate;
  switch(net_state)
    {
    case NET_STATE_FIRSTRUN:
      net_timeout_rxdata = NET_RXDATA_TIMEOUT;
      led_set(OVMS_LED_GRN,OVMS_LED_ON);
      led_set(OVMS_LED_RED,OVMS_LED_ON);
      led_start();
      net_timeout_goto = NET_STATE_START;
      net_timeout_ticks = 10; // Give everything time to start slowly
      break;
      
#ifdef OVMS_DIAGMODULE
    case NET_STATE_DIAGMODE:
      diag_initialise();
      break;
#endif // #ifdef OVMS_DIAGMODULE
      
    case NET_STATE_START:
      led_set(OVMS_LED_GRN,NET_LED_WAKEUP);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_timeout_goto = NET_STATE_HARDRESET;
      net_timeout_ticks = 20; // modem cold start takes 5 secs, warm restart takes 2 secs, 3 secs required for autobuad sync, 20 secs should be sufficient for everything
      net_msg_init();
      break;
    case NET_STATE_SOFTRESET:
      net_timeout_goto = 0;
      break;
      
    case NET_STATE_HARDRESET:
      led_set(OVMS_LED_GRN,OVMS_LED_ON);
      led_set(OVMS_LED_RED,OVMS_LED_ON);
      led_start();
      // Power modem down, up:
      modem_pwrkey();
      modem_pwrkey();
      net_timeout_goto = NET_STATE_SOFTRESET;
      net_timeout_ticks = 2;
      net_state_vchar = 0;
      net_msg_disconnected();
      net_cops_tries = 0; // Reset the COPS counter
      break;
      
    case NET_STATE_HARDSTOP:
      net_timeout_goto = NET_STATE_HARDSTOP2;
      net_timeout_ticks = 3;
      net_state_vchar = NETINIT_START;
      net_msg_disconnected();
      delay100(10);
      net_puts_rom("AT+CIPSHUT\r");
      break;
    case NET_STATE_HARDSTOP2:
      net_timeout_goto = NET_STATE_STOP;
      net_timeout_ticks = 2;
      net_state_vchar = 0;
      // Power modem down, up:
      modem_pwrkey();
      modem_pwrkey();
      break;
    case NET_STATE_STOP:
      led_set(OVMS_LED_GRN,OVMS_LED_OFF);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      CHECKPOINT(0xF0)
      reset_cpu();
      break;
      
    case NET_STATE_DOINIT:
      led_set(OVMS_LED_GRN,NET_LED_INITSIM1);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_timeout_goto = NET_STATE_HARDRESET;
      net_timeout_ticks = 95;
      net_state_vchar = 0;
#ifdef OVMS_INTERNALGPS
      // Using internal SIMx08 GPS:
      if ((net_fnbits & NET_FN_INTERNALGPS) != 0)
        {
        net_puts_rom(NET_INIT1_GPSON);
        }
      else
        {
        net_puts_rom(NET_INIT1);
        }
#else
      // Using external GPS from car:
      net_puts_rom(NET_INIT1);
#endif
      break;
    case NET_STATE_DOINIT2:
      led_set(OVMS_LED_GRN,NET_LED_INITSIM2);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_timeout_goto = NET_STATE_HARDRESET;
      net_timeout_ticks = 95;
      net_state_vchar = 0;
      net_puts_rom(NET_INIT2);
      break;
    case NET_STATE_DOINIT3:
      led_set(OVMS_LED_GRN,NET_LED_INITSIM3);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_timeout_goto = NET_STATE_HARDRESET;
      net_timeout_ticks = 35;
      net_state_vchar = 0;
      net_puts_rom(NET_INIT3);
      break;
    
    case NET_STATE_NETINITP:
      led_set(OVMS_LED_GRN,NET_LED_NETINIT);
      // Check retry count:
      if (--net_state_vint > 0)
        {
        led_set(OVMS_LED_RED,NET_LED_ERRGPRSRETRY);
        net_timeout_goto = NET_STATE_DONETINIT;
        }
      else
        {
        led_set(OVMS_LED_RED,NET_LED_ERRGPRSFAIL);
        net_timeout_goto = NET_STATE_SOFTRESET;
        }
      net_timeout_ticks = (NET_GPRS_RETRIES-net_state_vint)*5;
      break;
    
    case NET_STATE_NETINITCP:
      led_set(OVMS_LED_GRN,NET_LED_NETINIT);
      net_puts_rom("AT+CIPCLOSE\r");
      led_set(OVMS_LED_RED,NET_LED_ERRCONNFAIL);
      // Check retry count:
      if (--net_state_vint > 0)
        {
        net_timeout_goto = NET_STATE_DONETINITC;
        }
      else
        {
        net_timeout_goto = NET_STATE_SOFTRESET;
        }
      net_timeout_ticks = (NET_GPRS_RETRIES-net_state_vint)*5;
      break;
    case NET_STATE_DONETINITC:
      led_set(OVMS_LED_GRN,NET_LED_NETINIT);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_watchdog=0; // Disable watchdog, as we have connectivity
      net_reg = 0x05; // Assume connectivity (as COPS worked)
      net_timeout_goto = NET_STATE_SOFTRESET;
      net_timeout_ticks = 60;
      net_state_vchar = NETINIT_CLPORT;
      net_msg_disconnected();
      delay100(2);
      net_puts_rom("AT+CLPORT=\"TCP\",\"6867\"\r");
      net_state = NET_STATE_DONETINIT;
      break;
      
    case NET_STATE_DONETINIT:
      led_set(OVMS_LED_GRN,NET_LED_NETINIT);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_watchdog=0; // Disable watchdog, as we have connectivity
      net_reg = 0x05; // Assume connectivity (as COPS worked)
      p = par_get(PARAM_GPRSAPN);
      if ((p[0] != '\0') && inputs_gsmgprs()) // APN defined AND switch is set to GPRS mode
        {
        net_timeout_goto = NET_STATE_SOFTRESET;
        net_timeout_ticks = 60;
        net_state_vchar = NETINIT_START;
        net_msg_disconnected();
        delay100(2);
        net_puts_rom("AT+CIPSHUT\r");
        break;
        }
      else
        net_state = NET_STATE_READY;
      // N.B. Drop through without a break
    case NET_STATE_READY:
      led_set(OVMS_LED_GRN,NET_LED_READY);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_msg_sendpending = 0;
      net_timeout_goto = 0;
      net_state_vchar = 0;
      if ((net_reg != 0x01)&&(net_reg != 0x05))
        net_watchdog = NET_REG_TIMEOUT; // I need a network within 2 mins, else reset
      else
        net_watchdog = 0; // Disable net watchdog
      break;
    case NET_STATE_COPS:
      led_set(OVMS_LED_GRN,NET_LED_COPS);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      delay100(2);
      net_timeout_goto = NET_STATE_HARDRESET;
      net_timeout_ticks = 120;
      net_msg_disconnected();
      p = par_get(PARAM_GSMLOCK);
      if (*p==0)
        {
        net_puts_rom(NET_COPS);
        }
      else
        {
        net_puts_rom("AT+COPS=1,1,\"");
        net_puts_ram(p);
        net_puts_rom("\";+COPS?\r");
        }
      break;
    case NET_STATE_COPSSETTLE:
      net_timeout_ticks = 10;
      net_timeout_goto = NET_STATE_DONETINIT;
      break;
    case NET_STATE_COPSWAIT:
      led_set(OVMS_LED_GRN,NET_LED_COPS);
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
      net_timeout_goto = NET_STATE_COPSWDONE;
      net_timeout_ticks = 300;
      break;
    case NET_STATE_COPSWDONE:
      if (net_cops_tries++ < 20)
        {
        net_state_enter(NET_STATE_SOFTRESET); // Reset the entire async
        }
      else
        {
        net_state_enter(NET_STATE_HARDRESET); // Reset the entire async
        }
      break;
    }
  }

////////////////////////////////////////////////////////////////////////
// net_state_activity()
// State Model: Some async data has been received
// This is called to indicate to the state that a complete piece of async
// data has been received in net_buf (with length net_buf_pos, and mode
// net_buf_mode), and the data should be completely handled before
// returning.
//
void net_state_activity()
  {
  char *b;

  // Reset timeouts:
  net_buf_todotimeout = 0;
  net_timeout_rxdata = NET_RXDATA_TIMEOUT;

  CHECKPOINT(0x35)

  if (net_buf_mode == NET_BUF_SMS)
    {
    // An SMS has arrived, and net_caller has been primed
    if ((net_reg != 0x01)&&(net_reg != 0x05))
      { // Treat this as a network registration
      net_watchdog=0; // Disable watchdog, as we have connectivity
      net_reg = 0x05;
      led_set(OVMS_LED_RED,OVMS_LED_OFF);
    }
    CHECKPOINT(0x36)
    net_sms_in(net_caller,net_buf);
    CHECKPOINT(0x35)
    return;
    }
  else if (net_buf_mode != NET_BUF_CRLF)
    {
    // An IP data message has arrived
    CHECKPOINT(0x37)
    net_msg_in(net_buf);
    CHECKPOINT(0x35)
    // Getting GPRS data from the server means our connection was good
    net_state_vint = NET_GPRS_RETRIES; // Count-down for DONETINIT attempts
    return;
    }

  /*
  if ((net_buf_pos >= 8)&&
      (memcmppgm2ram(net_buf, "*PSUTTZ:", 8) == 0))
    {
    // We have a time source from the GSM provider
    // e.g.; *PSUTTZ: 2013, 12, 16, 15, 45, 17, "+32", 1
    return;
    }
   */

  switch (net_state)
    {
#ifdef OVMS_DIAGMODULE
    case NET_STATE_FIRSTRUN:
      if ((net_buf_pos >= 5)&&
          (memcmppgm2ram(net_buf, "SETUP", 5) == 0))
        {
        net_state_enter(NET_STATE_DIAGMODE);
        }
      break;
#endif // #ifdef OVMS_DIAGMODULE
    case NET_STATE_START:
      if ((net_buf_pos >= 2)&&(net_buf[0] == 'O')&&(net_buf[1] == 'K'))
        {
        // OK response from the modem
        led_set(OVMS_LED_RED,OVMS_LED_OFF);
        net_state_enter(NET_STATE_DOINIT);
        }
      break;
    case NET_STATE_DOINIT:
      if ((net_buf_pos >= 4)&&(net_buf[0]=='+')&&(net_buf[1]=='C')&&(net_buf[2]=='S')&&(net_buf[3]=='M'))
        {
        if (net_buf[strlen(net_buf)-1] != '1')
          {
          // The SIM card is not inserted
#ifdef OVMS_DIAGMODULE
          net_state_enter(NET_STATE_DIAGMODE);
#else
          led_set(OVMS_LED_RED,NET_LED_ERRSIM1);
          net_state_vchar = 1;
#endif //OVMS_DIAGMODULE
          }
        }
      else if ((net_state_vchar==0)&&(net_buf_pos >= 2)&&(net_buf[0] == 'O')&&(net_buf[1] == 'K'))
        {
        // The SIM card is inserted
        led_set(OVMS_LED_RED,OVMS_LED_OFF);
        net_state_enter(NET_STATE_DOINIT2);
        }
      break;
    case NET_STATE_DOINIT2:
      if ((net_buf_pos >= 16)&&(net_buf[0]=='8')&&(net_buf[1]=='9'))
        {
        // Looks like the ICCID
        strncpy(net_iccid,net_buf,MAX_ICCID);
        net_iccid[MAX_ICCID-1] = 0;
        }
#ifndef OVMS_NO_PHONEBOOKAP
      else if ((net_buf_pos >= 8)&&(net_buf[0]=='+')&&(net_buf[1]=='C')&&(net_buf[2]=='P')&&
               (net_buf[3]=='B')&&(net_buf[4]=='F')&&(net_buf[5]==':'))
        {
        net_phonebook(net_buf);
        }
#endif // OVMS_NO_PHONEBOOKAP
      else if ((net_buf_pos >= 8)&&(net_buf[0]=='+')&&(net_buf[1]=='C')&&(net_buf[2]=='P')&&(net_buf[3]=='I'))
        {
        if (net_buf[7] != 'R')
          {
          // The SIM card has some form of pin lock
          led_set(OVMS_LED_RED,NET_LED_ERRSIM2);
          net_state_vchar = 1;
          }
        }
      else if ((net_state_vchar==0)&&(net_buf_pos >= 2)&&(net_buf[0] == 'O')&&(net_buf[1] == 'K'))
        {
        // The SIM card has no pin lock
        led_set(OVMS_LED_RED,OVMS_LED_OFF);
        net_state_enter(NET_STATE_DOINIT3);
        }
      break;
    case NET_STATE_DOINIT3:
      if ((net_buf_pos >= 6)&&(net_buf[0] == '+')&&(net_buf[1] == 'I')&&(net_buf[2] == 'P')&&(net_buf[3] == 'R')&&(net_buf[6] != '9'))
        {
        // +IPR != 9600
        // SET IPR (baudrate)
        net_puts_rom(NET_IPR_SET);
        }
      else if ((net_buf_pos >= 2)&&(net_buf[0] == 'O')&&(net_buf[1] == 'K'))
        {
        led_set(OVMS_LED_RED,OVMS_LED_OFF);
        net_state_enter(NET_STATE_COPS);
        }
      break;
    case NET_STATE_COPS:
      if ((net_buf_pos >= 2)&&(net_buf[0] == 'O')&&(net_buf[1] == 'K'))
        {
        net_state_vint = NET_GPRS_RETRIES; // Count-down for DONETINIT attempts
        net_cops_tries = 0; // Successfully out of COPS
        net_state_enter(NET_STATE_COPSSETTLE); // COPS reconnect was OK
        }
      else if ( ((net_buf_pos >= 5)&&(net_buf[0] == 'E')&&(net_buf[1] == 'R')) ||
              (memcmppgm2ram(net_buf, "+CME ERROR", 10) == 0) )
        {
        net_state_enter(NET_STATE_COPSWAIT); // Try to wait a bit to see if we get a CREG
        }
      else if (memcmppgm2ram(net_buf, "+COPS:", 6) == 0)
        {
        // COPS network registration
        b = firstarg(net_buf, '\"');
        if (b != NULL)
          {
          b = nextarg(b);
          if (b != NULL)
            {
            strncpy(car_gsmcops,b,8);
            car_gsmcops[8] = 0;
            }
          }
        }
      break;
    case NET_STATE_COPSWAIT:
      if (memcmppgm2ram(net_buf, "+CREG", 5) == 0)
        { // "+CREG" Network registration
        if (net_buf[8]==',')
          net_reg = net_buf[9]&0x07; // +CREG: 1,x
        else
          net_reg = net_buf[7]&0x07; // +CREG: x
        if ((net_reg == 0x01)||(net_reg == 0x05)) // Registered to network?
          {
          net_state_vint = NET_GPRS_RETRIES; // Count-down for DONETINIT attempts
          net_cops_tries = 0; // Successfully out of COPS
          net_state_enter(NET_STATE_DONETINIT); // COPS reconnect was OK
          }
        }
      break;
    case NET_STATE_DONETINIT:
      if ((net_buf_pos >= 2)&&(net_buf[0] == 'E')&&(net_buf[1] == 'R') ||
              (memcmppgm2ram(net_buf, "+CME ERROR", 10) == 0))
        {
        if ((net_state_vchar == NETINIT_CSTT)||
                (net_state_vchar == NETINIT_CIICR))// ERROR response to AT+CSTT OR AT+CIICR
          {
          // try registering, then setting up GPRS again, after short pause
          net_state_enter(NET_STATE_NETINITP);
          }
        else if (net_state_vchar == NETINIT_CIFSR) // ERROR response to AT+CIFSR
          {
          // This is a nasty case. The GPRS has locked up.
          // The only solution I can find is a hard reset of the modem
          net_state_enter(NET_STATE_HARDRESET);
          }
        }
      else if ((net_buf_pos >= 2)&&
          (((net_buf[0] == 'O')&&(net_buf[1] == 'K')))|| // OK
          (((net_buf[0] == 'S')&&(net_buf[1] == 'H')))||  // SHUT OK
          (net_state_vchar == NETINIT_CIFSR)) // Local IP address
        {
        net_buf_pos = 0;
        net_timeout_ticks = 30;
        net_link = 0;
        delay100(2);
        switch (++net_state_vchar)
          {
          case NETINIT_CGDCONT:
            net_puts_rom("AT+CGDCONT=1,\"IP\",\"");
            net_puts_ram(par_get(PARAM_GPRSAPN));
            net_puts_rom("\"\r");
            break;
          case NETINIT_CSTT:
            net_puts_rom("AT+CSTT=\"");
            net_puts_ram(par_get(PARAM_GPRSAPN));
            net_puts_rom("\",\"");
            net_puts_ram(par_get(PARAM_GPRSUSER));
            net_puts_rom("\",\"");
            net_puts_ram(par_get(PARAM_GPRSPASS));
            net_puts_rom("\"\r");
            break;
          case NETINIT_CIICR:
            led_set(OVMS_LED_GRN,NET_LED_NETAPNOK);
            net_puts_rom("AT+CIICR\r");
            break;
          case NETINIT_CIPHEAD:
            net_puts_rom("AT+CIPHEAD=1\r");
            break;
          case NETINIT_CIFSR:
            net_puts_rom("AT+CIFSR\r");
            break;
          case NETINIT_CDNSCFG:
            b = par_get(PARAM_GPRSDNS);
            if (*b == 0)
              net_puts_rom("AT\r");
            else
              {
              net_puts_rom("AT+CDNSCFG=\"");
              net_puts_ram(b);
              net_puts_rom("\"\r");
              }
            break;
          case NETINIT_CLPORT:
            net_puts_rom("AT+CLPORT=\"TCP\",\"6867\"\r");
            break;
          case NETINIT_CIPSTART:
            led_set(OVMS_LED_GRN,NET_LED_NETCALL);
            net_puts_rom("AT+CIPSTART=\"TCP\",\"");
            net_puts_ram(par_get(PARAM_SERVERIP));
            net_puts_rom("\",\"6867\"\r");
            break;
          case NETINIT_CONNECTING:
            net_state_enter(NET_STATE_READY);
            break;
          }
        }
      else if ((net_buf_pos>=7)&&
               (memcmppgm2ram(net_buf, "+CREG: 0", 8) == 0))
        { // Lost network connectivity during NETINIT
        net_state_enter(NET_STATE_SOFTRESET);
        }
      else if (memcmppgm2ram(net_buf, "+PDP: DEACT", 11) == 0)
        { // PDP couldn't be activated - try again...
        net_state_enter(NET_STATE_SOFTRESET);
        }
      break;
      
      
    case NET_STATE_READY:
      if (memcmppgm2ram(net_buf, "+CREG", 5) == 0)
        {
        // "+CREG" Network registration: either from...
        if (net_buf[8]==',')
          net_reg = net_buf[9]&0x07; // ...CMD: "+CREG: 1,x"
        else
          net_reg = net_buf[7]&0x07; // ...URC: "+CREG: x"
        // 1 = Registered, home network
        // 5 = Registered, roaming
        if ((net_reg == 0x01)||(net_reg == 0x05)) // Registered to network?
          {
          net_watchdog=0; // Disable watchdog, as we have connectivity
          led_set(OVMS_LED_RED,OVMS_LED_OFF);
          }
        else if (net_watchdog == 0)
          {
          net_watchdog = NET_REG_TIMEOUT; // We need connectivity within 120 seconds
          led_set(OVMS_LED_RED,NET_LED_ERRLOSTSIG);
          }
        }
      else if (memcmppgm2ram(net_buf, "+CLIP", 5) == 0)
        { // Incoming CALL
        if ((net_reg != 0x01)&&(net_reg != 0x05))
          { // Treat this as a network registration
          net_watchdog=0; // Disable watchdog, as we have connectivity
          net_reg = 0x05;
          led_set(OVMS_LED_RED,OVMS_LED_OFF);
          }
        delay100(1);
        net_puts_rom(NET_HANGUP);
        }
      else if (memcmppgm2ram(net_buf, "+CCLK", 5) == 0)
        {
        // local clock update
        // e.g.; +CCLK: "13/12/16,22:01:39+32"
        if ((net_fnbits & NET_FN_CARTIME)>0)
          {
          unsigned long newtime = datestring_to_timestamp(net_buf+7);
          if (newtime != car_time)
            {
            // Need to adjust the car_time
            if (newtime > car_time)
              {
              unsigned long diff = newtime - car_time;
              if (car_parktime!=0) { car_parktime += diff; }
              car_time += diff;
              }
            else
              {
              unsigned long diff = car_time - newtime;
              if (car_parktime!=0) { car_parktime -= diff; }
              car_time -= diff;
              }
            }
          }
        }
#if defined(OVMS_INTERNALGPS) && defined(OVMS_SIMCOM_SIM908)
      else if ((memcmppgm2ram(net_buf, "2,", 2) == 0)&&
               ((net_fnbits & NET_FN_INTERNALGPS)>0))
        {
        // Incoming GPS coordinates
        // NMEA format $GPGGA: Global Positioning System Fixed Data
        // 2,<Time>,<Lat>,<NS>,<Lon>,<EW>,<Fix>,<SatCnt>,<HDOP>,<Alt>,<Unit>,...

        long lat, lon;
        char ns, ew;
        char fix, satcnt;
        int alt;

        // Parse string:
        if (b = firstarg(net_buf+2, ','))
          ;                                     // Time
        if (b = nextarg(b))
          lat = gps2latlon(b);                  // Latitude
        if (b = nextarg(b))
          ns = *b;                              // North / South
        if (b = nextarg(b))
          lon = gps2latlon(b);                  // Longitude
        if (b = nextarg(b))
          ew = *b;                              // East / West
        if (b = nextarg(b))
          fix = *b;                             // Fix (0/1)
        if (b = nextarg(b))
          satcnt = atoi(b);                     // Satellite count
        if (b = nextarg(b))
          ;                                     // HDOP
        if (b = nextarg(b))
          alt = atoi(b);                        // Altitude

        if (b)
          {
          // data set complete, store:

          // upper two bits for fix mode (0-2), 6 bits for satcnt:
          car_gpslock = ((fix & 0x03) << 6) + satcnt;

          if (GPS_LOCK())
            {
            if (ns == 'S') lat = ~lat;
            if (ew == 'W') lon = ~lon;

            car_latitude = lat;
            car_longitude = lon;
            car_altitude = alt;

            car_stale_gps = 120; // Reset stale indicator
            }
          else
            {
            car_stale_gps = 0;
            }
         }

      }
    else if ((memcmppgm2ram(net_buf, "64,", 3) == 0)&&
             ((net_fnbits & NET_FN_INTERNALGPS)>0))
      {
      // Incoming GPS coordinates
      // NMEA format $GPVTG: Course over ground
      // 64,<Course>,<Ref>,...

      int dir;

      // Parse string:
      if (b = firstarg(net_buf+3, ','))
        dir = atoi(b);                      // Course

      if (b)
        {
        // data set complete, store:
        if (GPS_LOCK())
          {
          car_direction = dir;
          }
        }

      }
#elif defined(OVMS_INTERNALGPS) && defined(OVMS_SIMCOM_SIM808)
      else if ((memcmppgm2ram(net_buf, "+CGNSINF:", 9) == 0)&&
               ((net_fnbits & NET_FN_INTERNALGPS)>0))
        {
        // Incoming GPS coordinates
        // +CGNSINF: <GNSS run status>,<Fix status>, <UTC date & Time>,<Latitude>,<Longitude>,
        // <MSL Altitude>,<Speed Over Ground>, <Course Over Ground>,
        // <Fix Mode>,<Reserved1>,<HDOP>,<PDOP>, <VDOP>,<Reserved2>,<GPS Satellites in View>,
        // <GNSS Satellites Used>,<GLONASS Satellites in View>,<Reserved3>,<C/N0 max>,<HPA>,<VPA>
        long lat, lon;
        char fix, satcnt, i;
        int alt;
        int dir;
      
        // Parse string:
        if (b = firstarg(net_buf+9, ','))
          ;                                     // GNSS run status
        if (b = nextarg(b))
          fix = *b;                             // GNSS Fix status
        if (b = nextarg(b))
          ;                                     // UTC date & time
        if (b = nextarg(b))
          lat = gps2latlon(b);                  // Latitude
        if (b = nextarg(b))
          lon = gps2latlon(b);                  // Longitude
        if (b = nextarg(b))
          alt = atoi(b);                        // Altitude
        if (b = nextarg(b))
          ;                                     // Speed over ground
        if (b = nextarg(b))
          dir = atoi(b);                        // Course over ground
        for (i=7; i; i--)
          b = nextarg(b);                       // skip 7 fields
        if (b = nextarg(b))
          satcnt = atoi(b);                     // Satellite count

        if (b)
          {
          // data set complete, store:

          car_gpslock = ((fix & 0x03) << 6) + satcnt;

          if (GPS_LOCK())
            {
            car_latitude = lat;
            car_longitude = lon;
            car_altitude = alt;
            car_direction = dir;
          
            car_stale_gps = 120; // Reset stale indicator
            }
          else
            {
            car_stale_gps = 0;
            }
          }
        }
#endif
      else if (memcmppgm2ram(net_buf, "CONNECT OK", 10) == 0)
        {
        if (net_link == 0)
          {
          led_set(OVMS_LED_GRN,NET_LED_READYGPRS);
          net_msg_start();
          net_msg_register();
          net_msg_send();
          }
        net_link = 1;
        }
      else if (memcmppgm2ram(net_buf, "STATE: ", 7) == 0)
        { // Incoming CIPSTATUS
        if (memcmppgm2ram(net_buf, "STATE: CONNECT OK", 17) == 0)
          {
          if (net_link == 0)
            {
            led_set(OVMS_LED_GRN,NET_LED_READYGPRS);
            net_msg_start();
            net_msg_register();
            net_msg_send();
            }
          net_link = 1;
          }
        else if (memcmppgm2ram(net_buf, "STATE: TCP CONNECTING", 21) == 0)
          {
          // Connection in progress, ignore it...
          }
        else if (memcmppgm2ram(net_buf, "STATE: TCP CLOSED", 17) == 0)
          {
          // Re-initialize TCP socket, after short pause
          net_msg_disconnected();
          net_state_enter(NET_STATE_NETINITCP);
          }
        else
          {
          net_link = 0;
          led_set(OVMS_LED_GRN,NET_LED_READY);
          if ((net_reg == 0x01)||(net_reg == 0x05))
            {
            // We have a GSM network, but CIPSTATUS is not up
            net_msg_disconnected();
            // try setting up GPRS again, after short pause
            net_state_enter(NET_STATE_NETINITP);
            }
          }
        }
      else if (memcmppgm2ram(net_buf, "+CSQ:", 5) == 0)
        {
        // Signal Quality
          if (net_buf[8]==',')  // two digits
             net_sq = (net_buf[6]&0x07)*10 + (net_buf[7]&0x07);
          else net_sq = net_buf[6]&0x07;
        }
      else if ( (memcmppgm2ram(net_buf, "SEND OK", 7) == 0) ||
                (memcmppgm2ram(net_buf, "DATA ACCEPT", 11) == 0) )
        {
        // CIPSEND success response
        net_msg_sendpending = -1; // 1s modem VBAT recharge pause
        }
      else if ( (memcmppgm2ram(net_buf, "CLOSED", 6) == 0) ||
                (memcmppgm2ram(net_buf, "CONNECT FAIL", 12) == 0) ||
                (net_msg_sendpending && (
                  (memcmppgm2ram(net_buf, "SEND FAIL", 9) == 0) ||
                  (memcmppgm2ram(net_buf, "ERROR", 5) == 0) ||
                  (memcmppgm2ram(net_buf, "+CME ERROR", 10) == 0)))
              )
        {
        // TCP connection has been lost:
        // Re-initialize TCP socket, after short pause
        net_msg_disconnected();
        net_state_enter(NET_STATE_NETINITCP);
        }
      else if ( (memcmppgm2ram(net_buf, "+PDP: DEACT", 11) == 0) ||
                (memcmppgm2ram(net_buf, "ERROR", 5) == 0) ||
                (memcmppgm2ram(net_buf, "+CME ERROR", 10) == 0)
              )
        {
        // GPRS context has been lost:
        // Re-initialize GPRS network and TCP socket, after short pause
        net_msg_disconnected();
        net_state_enter(NET_STATE_NETINITP);
        }
      else if ( (memcmppgm2ram(net_buf, "RDY", 4) == 0)||
                (memcmppgm2ram(net_buf, "+CFUN:", 6) == 0) )
        {
        // Modem crash/reset: do full re-init
        net_msg_disconnected();
        net_state_enter(NET_STATE_START);
        }
      else if ( (memcmppgm2ram(net_buf, "NORMAL POWER DOWN", 17) == 0) )
        {
        // Modem power down detected: power up, do full re-init
        modem_pwrkey();
        net_state_enter(NET_STATE_START);
        }
      else if (memcmppgm2ram(net_buf, "+CUSD:", 6) == 0)
        {
        // reply MMI/USSD command result:
        net_msg_reply_ussd(net_buf, net_buf_pos);
        }
      break;
      
      
#ifdef OVMS_DIAGMODULE
    case NET_STATE_DIAGMODE:
      diag_activity(net_buf,net_buf_pos);
      break;
#endif // #ifdef OVMS_DIAGMODULE
    }
  }


////////////////////////////////////////////////////////////////////////
// net_idlepoll()
// 
// This function is called from the main loop after each net_poll().
// As net_poll() frees the net_msg_sendpending semaphore, this is
// the place to send queued notifications without interfering with
// the per second tickers.
//
void net_idlepoll(void)
  {
  char stat;
  char cmd[5];

#ifdef OVMS_DIAGMODULE
  if ((net_state == NET_STATE_DIAGMODE))
    ; // disable connection check
  else
#endif // OVMS_DIAGMODULE
    // Connection ready & available?
    if ((net_state!=NET_STATE_READY) || ((net_reg!=0x01)&&(net_reg!=0x05)))
      return;
  
  if (!MODEM_READY())
    return;

  
  /*************************************************************
   * PROCESS BUFFERED COMMAND
   */

  if ((net_msg_cmd_code!=0) && (net_msg_serverok==1))
    {
    net_msg_cmd_do();
    return;
    }

  
  /*************************************************************
   * SEND IP NOTIFICATIONS
   */

#ifndef OVMS_NO_ERROR_NOTIFY
  if ((net_notify_errorcode>0) && (net_msg_serverok==1))
    {
    if (net_notify_errorcode > 0)
      {
      net_msg_erroralert(net_notify_errorcode, net_notify_errordata);
      }
    net_notify_errorcode = 0;
    net_notify_errordata = 0;
    return;
    }
#endif //OVMS_NO_ERROR_NOTIFY

  if (((net_notify & NET_NOTIFY_NETPART)>0)
          && (net_msg_serverok==1))
    {

#ifndef OVMS_NO_VEHICLE_ALERTS
    if ((net_notify & NET_NOTIFY_NET_ALARM)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_ALARM); // Clear notification flag
      net_msg_alert(ALERT_ALARM);
      return;
      }
    else
#endif //OVMS_NO_VEHICLE_ALERTS
      
    if ((net_notify & NET_NOTIFY_NET_CHARGE)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_CHARGE); // Clear notification flag
      if (net_notify_suppresscount==0)
        {
        // execute CHARGE ALERT command:
        net_msg_cmd_code = 6;
        net_msg_cmd_msg = cmd;
        net_msg_cmd_msg[0] = 0;
        net_msg_cmd_do();
        }
      return;
      }
    
    else if ((net_notify & NET_NOTIFY_NET_12VLOW)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_12VLOW); // Clear notification flag
      if (net_fnbits & NET_FN_12VMONITOR) net_msg_alert(ALERT_12VLOW);
      return;
      }
    
#ifndef OVMS_NO_VEHICLE_ALERTS
    else if ((net_notify & NET_NOTIFY_NET_TRUNK)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_TRUNK); // Clear notification flag
      net_msg_alert(ALERT_TRUNK);
      return;
      }
#endif //OVMS_NO_VEHICLE_ALERTS
    
    else if ((net_notify & NET_NOTIFY_NET_CARON)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_CARON); // Clear notification flag
      net_msg_alert(ALERT_CARON);
      return;
      }
    
    else if ((net_notify & NET_NOTIFY_NET_UPDATE)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_UPDATE | NET_NOTIFY_NET_STAT |
              NET_NOTIFY_NET_STREAM); // Clear all covered notifications
      stat = 2;
      stat = net_msgp_stat(stat);
      stat = net_msgp_environment(stat);
      stat = net_msgp_gps(stat);
      stat = net_msgp_group(stat,1);
      stat = net_msgp_group(stat,2);
#ifndef OVMS_NO_TPMS
      stat = net_msgp_tpms(stat);
#endif
      stat = net_msgp_firmware(stat);
      stat = net_msgp_capabilities(stat);
      if (stat != 2)
        net_msg_send();
      return;
      }
    
    else if ((net_notify & NET_NOTIFY_NET_STAT)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_STAT); // Clear notification flag
      stat = 2;
      stat = net_msgp_environment(stat);
      stat = net_msgp_stat(stat);
      if (stat != 2)
        net_msg_send();
      return;
      }
    
    else if ((net_notify & NET_NOTIFY_NET_STREAM)>0)
      {
      net_notify &= ~(NET_NOTIFY_NET_STREAM); // Clear notification flag
      if (net_msgp_gps(2) != 2)
        net_msg_send();
      return;
      }
    
    } // if NET_NOTIFY_NETPART

  
  /*************************************************************
   * SEND SMS NOTIFICATIONS
   */
  if ((net_notify & NET_NOTIFY_SMSPART)>0)
    {
    net_assert_caller(NULL); // set net_caller to PARAM_REGPHONE
    
#ifndef OVMS_NO_VEHICLE_ALERTS
    if ((net_notify & NET_NOTIFY_SMS_ALARM)>0)
      {
      net_notify &= ~(NET_NOTIFY_SMS_ALARM); // Clear notification flag
      net_sms_alert(net_caller, ALERT_ALARM);
      return;
      }
    else
#endif //OVMS_NO_VEHICLE_ALERTS
    
    if ((net_notify & NET_NOTIFY_SMS_CHARGE)>0)
      {
      net_notify &= ~(NET_NOTIFY_SMS_CHARGE); // Clear notification flag
      if (net_notify_suppresscount==0)
        {
          stp_rom(cmd, "STAT");
          net_sms_in(net_caller, cmd);
        }
      return;
      }
    
    else if ((net_notify & NET_NOTIFY_SMS_12VLOW)>0)
      {
      net_notify &= ~(NET_NOTIFY_SMS_12VLOW); // Clear notification flag
      if (net_fnbits & NET_FN_12VMONITOR) net_sms_alert(net_caller, ALERT_12VLOW);
      return;
      }

#ifndef OVMS_NO_VEHICLE_ALERTS
    else if ((net_notify & NET_NOTIFY_SMS_TRUNK)>0)
      {
      net_notify &= ~(NET_NOTIFY_SMS_TRUNK); // Clear notification flag
      net_sms_alert(net_caller, ALERT_TRUNK);
      return;
      }
#endif //OVMS_NO_VEHICLE_ALERTS

    else if ((net_notify & NET_NOTIFY_SMS_CARON)>0)
      {
      net_notify &= ~(NET_NOTIFY_SMS_CARON); // Clear notification flag
      net_sms_alert(net_caller, ALERT_CARON);
      return;
      }

    } // if NET_NOTIFY_SMSPART

  }


////////////////////////////////////////////////////////////////////////
// net_state_ticker1()
// State Model: Per-second ticker
// This function is called approximately once per second, and gives
// the state a timeslice for activity.
//
void net_state_ticker1(void)
  {
  CHECKPOINT(0x38)

#ifndef OVMS_NO_ERROR_NOTIFY
  // Time out error codes
  if (net_notify_lastcount>0)
    {
    net_notify_lastcount--;
    if (net_notify_lastcount == 0)
      {
      net_notify_lasterrorcode = 0;
      }
    }
#endif //OVMS_NO_ERROR_NOTIFY

  switch (net_state)
    {
    case NET_STATE_START:
      if (net_timeout_ticks == 10)
        {
        // No answer yet, try toggling the modem power:
        modem_pwrkey();
        delay100(50);
        }
      else if (net_timeout_ticks < 5)
        {
        // We are about to timeout, so let's set the error code...
        led_set(OVMS_LED_RED,NET_LED_ERRMODEM);
        }
#ifdef OVMS_INTERNALGPS
      // Using internal SIMx08 GPS:
      if ((net_fnbits & NET_FN_INTERNALGPS) != 0)
        {
        net_puts_rom(NET_WAKEUP_GPSON);
        }
      else
        {
        net_puts_rom(NET_WAKEUP_GPSOFF);
        }
#else
      // Using external GPS from car:
      net_puts_rom(NET_WAKEUP);
#endif
      break;
    case NET_STATE_DOINIT:
      if ((net_timeout_ticks % 3)==0)
        net_puts_rom(NET_INIT1);
      break;
    case NET_STATE_DOINIT2:
      if ((net_timeout_ticks % 3)==0)
        net_puts_rom(NET_INIT2);
      break;
    case NET_STATE_DOINIT3:
      if ((net_timeout_ticks % 3)==0)
        net_puts_rom(NET_INIT3);
      break;
    case NET_STATE_COPS:
      if (net_timeout_ticks < 20)
        {
        // We are about to timeout, so let's set the error code...
        led_set(OVMS_LED_RED,NET_LED_ERRCOPS);
        }
      break;
    case NET_STATE_COPSWAIT:
      if ((net_timeout_ticks % 10)==0)
        {
        net_wait4modem();
        net_puts_rom(NET_CREG_STATUS);
        while(vUARTIntTxBufDataCnt>0); // Wait for TX flush
        delay5(2); // Wait for result
        }
      break;
    case NET_STATE_SOFTRESET:
      net_state_enter(NET_STATE_FIRSTRUN);
      break;

    case NET_STATE_READY:

      if (net_buf_mode != NET_BUF_CRLF)
        {
        if (net_buf_todotimeout == 1)
          {
          // Timeout waiting for arrival of SMS/IP data
          net_buf_todotimeout = 0;
          net_buf_pos = 0;
          net_buf_mode = NET_BUF_CRLF;
          net_state_enter(NET_STATE_COPS); // Reset network connection
          return;
          }
        else if (net_buf_todotimeout > 1)
          net_buf_todotimeout--;
        }

      if (net_watchdog > 0)
        {
        // Timeout?
        if (--net_watchdog == 0)
          {
          net_state_enter(NET_STATE_COPS); // Reset network connection
          return;
          }
        }

      if (net_msg_sendpending)
        {
        if (++net_msg_sendpending > NET_IPACK_TIMEOUT)
          {
          // IPSEND ACK timeout:
          if (net_watchdog == 0)
            net_state_enter(NET_STATE_NETINITCP); // Reopen TCP connection
          else
            net_state_enter(NET_STATE_DONETINIT); // Reset GPRS link
          return;
          }
        }

#ifdef OVMS_INTERNALGPS
        // Request internal SIM908/808 GPS coordinates
        // every three seconds while car is on,
        // else once every minute (to trace theft / transportation)
        if ( (((car_doors1bits.CarON) && ((net_granular_tick % 3) == 1))
                || ((net_granular_tick % 60) == 55))
                && MODEM_READY()
                && ((net_fnbits & NET_FN_INTERNALGPS) > 0))
          {
          net_puts_rom(NET_REQGPS);
          while(vUARTIntTxBufDataCnt>0); // Wait for TX flush
          delay5(15); // Wait for result to begin
          }
#endif

      // ...case NET_STATE_READY + registered...
      if ((net_reg == 0x01)||(net_reg == 0x05))
        {
        // GPS location streaming: every three seconds, immediately after
        // retrieving internal GPS coordinates (if used).
        // IMPORTANT: do not raise frequency! Modem will power off due to
        // over current on cell switches in bad coverage areas!
        // (possible HW design flaw -- modem can surge up to 2A on GPRS sends)
        if ((car_speed>0) &&
            (sys_features[FEATURE_STREAM]==1) &&
            (net_apps_connected>0) &&
            // every second if we get GPS from the car, else every odd second:
            (((net_fnbits & NET_FN_INTERNALGPS) == 0)
              || ((net_granular_tick % 3) == 1)) )
          {
          // Car moving, and streaming on, apps connected, and not sending
          net_req_notification(NET_NOTIFY_STREAM);
          }
        } // if ((net_reg == 0x01)||(net_reg == 0x05))

      // Reset 12V calibration?
      if (car_doors5bits.Charging12V)
        {
        car_12vline_ref = 0;
        }

      break;

#ifdef OVMS_DIAGMODULE
    case NET_STATE_DIAGMODE:
      diag_ticker();
      break;
#endif // #ifdef OVMS_DIAGMODULE
    }
  }

////////////////////////////////////////////////////////////////////////
// net_state_ticker30()
// State Model: 30-second ticker
// This function is called approximately once per 30 seconds (since state
// was first entered), and gives the state a timeslice for activity.
//
void net_state_ticker30(void)
  {
  CHECKPOINT(0x39)

  switch (net_state)
    {
    case NET_STATE_READY:
      // Request network status + clock + signal quality
      // once per minute, offset 30 seconds to ticker60 (even second)
      if ((net_granular_tick % 60) != 0)
        {
        if (!MODEM_READY())
          {
          net_granular_tick -= 2; // Try again in 2 seconds...
          }
        else
          {
          net_puts_rom(NET_CREG_CIPSTATUS);
          while(vUARTIntTxBufDataCnt>0); // Wait for TX flush
          delay5(2); // Wait for result start
          }
        }
      break;
    }
  }

////////////////////////////////////////////////////////////////////////
// net_state_ticker60()
// State Model: Per-minute ticker
// This function is called approximately once per minute (since state
// was first entered), and gives the state a timeslice for activity.
//
void net_state_ticker60(void)
  {
  CHECKPOINT(0x3A)

#ifdef OVMS_HW_V2

  // Take 12v reading:
  if (car_12vline == 0)
  {
    // first reading:
    car_12vline = inputs_voltage()*10;
    car_12vline_ref = 0;
  }
  else
  {
    // filter peaks/misreadings:
    car_12vline = ((int)car_12vline + (int)(inputs_voltage()*10) + 1) / 2;

    // OR direct reading to test A/D converter fix: (failed...)
    //car_12vline = inputs_voltage()*10;
  }

  // Calibration: take reference voltage after charging
  //    Note: ref value 0 is "charging"
  //          ref value 1..CALMDOWN_TIME is calmdown counter
  if (car_doors5bits.Charging12V)
  {
    // charging now
  }
  else if (car_12vline_ref < BATT_12V_CALMDOWN_TIME)
  {
    // calmdown phase:
    if (car_doors1bits.CarON)
    {
      // car has been turned ON during calmdown; reset timer:
      car_12vline_ref = 0;
    }
    else
    {
      // wait CALMDOWN_TIME minutes after end of charge:
      car_12vline_ref++;
    }
  }
  else if ((car_12vline_ref == BATT_12V_CALMDOWN_TIME) && !car_doors1bits.CarON)
  {
    // calmdown done & car off: take new ref voltage & reset alert:
    car_12vline_ref = car_12vline;
    can_minSOCnotified &= ~CAN_MINSOC_ALERT_12V;
  }

  // Check voltage if ref is valid:
  if (car_12vline_ref > BATT_12V_CALMDOWN_TIME)
  {
    if (car_doors1bits.CarON)
    {
      // Reset 12V alert if car is on
      // because DC/DC system charges 12V from main battery
      can_minSOCnotified &= ~CAN_MINSOC_ALERT_12V;
    }
    else
    {
      // Car is off, trigger alert if necessary

      // Info: healthy lead/acid discharge depth is ~ nom - 1.0 V
      //        ref is ~ nom + 0.5 V

      // Trigger 12V alert if voltage <= ref - 1.6 V:
      if (!(can_minSOCnotified & CAN_MINSOC_ALERT_12V)
              && (car_12vline <= (car_12vline_ref - 16)))
      {
        can_minSOCnotified |= CAN_MINSOC_ALERT_12V;
        net_req_notification(NET_NOTIFY_12VLOW);
      }

      // Reset 12V alert if voltage >= ref - 1.0 V:
      else if ((can_minSOCnotified & CAN_MINSOC_ALERT_12V)
              && (car_12vline >= (car_12vline_ref - 10)))
      {
        can_minSOCnotified &= ~CAN_MINSOC_ALERT_12V;
        net_req_notification(NET_NOTIFY_12VLOW);
      }
    }
  }

#endif

  switch (net_state)
    {
    case NET_STATE_READY:
#ifdef OVMS_LOGGINGMODULE
      if ((net_msg_serverok)&&(logging_haspending() > 0))
        {
        if (!MODEM_READY())
          {
          net_granular_tick -= 4; // Try again in 4 seconds...
          }
        else
          {
          net_msg_start();
          logging_sendpending();
          net_msg_send();
          return;
          }
        }
#endif // #ifdef OVMS_LOGGINGMODULE
      
      // Send standard update
      // once per minute while Apps are connected
      if (net_apps_connected>0)
        net_req_notification(NET_NOTIFY_UPDATE);
      
      // Toggle modem state initialisation flag (?):
      net_state_vchar = net_state_vchar ^ 1;
      
      break;
    }
  }

////////////////////////////////////////////////////////////////////////
// net_state_ticker300()
// State Model: Per-5-minute ticker
// This function is called approximately once per five minutes (since
// state was first entered), and gives the state a timeslice for activity.
//
void net_state_ticker300(void)
  {
  CHECKPOINT(0x3B)

  switch (net_state)
    {
    }
  }

////////////////////////////////////////////////////////////////////////
// net_state_ticker600()
// State Model: Per-10-minute ticker
// This function is called approximately once per ten minutes (since
// state was first entered), and gives the state a timeslice for activity.
//
void net_state_ticker600(void)
  {
  char *p;
  BOOL carbusy = ((car_chargestate==1)||    // Charging
                  (car_chargestate==2)||    // Topping off
                  (car_chargestate==15)||   // Heating
                  ((car_doors1&0x80)>0));   // Car On

  CHECKPOINT(0x3C)

  switch (net_state)
    {
    case NET_STATE_READY:
#ifdef OVMS_SOCALERT
      if ((car_SOC<car_SOCalertlimit)&&((car_doors1 & 0x80)==0)) // Car is OFF, and SOC<car_SOCalertlimit
        {
        if (net_socalert_msg==0)
          {
          if ((net_msg_serverok) && MODEM_READY())
            {
            if (net_fnbits & NET_FN_SOCMONITOR) net_msg_alert(ALERT_SOCLOW);
            net_socalert_msg = 72; // 72x10mins = 12hours
            }
          }
        else
          net_socalert_msg--;
        if (net_socalert_sms==0)
          {
          p = par_get(PARAM_REGPHONE);
          if (net_fnbits & NET_FN_SOCMONITOR) net_sms_alert(p, ALERT_SOCLOW);
          net_socalert_sms = 72; // 72x10mins = 12hours
          }
        else
          net_socalert_sms--;
        }
      else
        {
        net_socalert_sms = 6;         // Check in 1 hour
        net_socalert_msg = 6;         // Check in 1 hour
        }
#endif //#ifdef OVMS_SOCALERT
      
      // Send standard update
      // every 10 minutes if no Apps are connected and car is busy
      if ((net_msg_serverok)&&(net_apps_connected==0)&&carbusy)
        net_req_notification(NET_NOTIFY_UPDATE);
      
      break;
    }
  }

////////////////////////////////////////////////////////////////////////
// net_state_ticker3600()
// State Model: Per-hour ticker
// This function is called approximately once per hour (since
// state was first entered), and gives the state a timeslice for activity.
//
void net_state_ticker3600(void)
  {
  BOOL carbusy = ((car_chargestate==1)||    // Charging
                  (car_chargestate==2)||    // Topping off
                  (car_chargestate==15)||   // Heating
                  ((car_doors1&0x80)>0));   // Car On

  CHECKPOINT(0x3D)

  switch (net_state)
    {
    case NET_STATE_READY:
      // Send standard update
      // once per hour if no Apps are connected and car is not busy
      if ((net_msg_serverok)&&(net_apps_connected==0)&&(!carbusy))
        net_req_notification(NET_NOTIFY_UPDATE);
      
      break;
    }
  }

////////////////////////////////////////////////////////////////////////
// net_ticker()
// This function is an entry point from the main() program loop, and
// gives the NET framework a ticker call approximately once per second.
// It is used to internally generate the other net_state_ticker*() calls.
//
void net_ticker(void)
  {
  // This ticker is called once every second

  CHECKPOINT(0x3E)

  if (net_notify_suppresscount>0) net_notify_suppresscount--;
  net_granular_tick++;
  if ((net_timeout_goto > 0)&&(net_timeout_ticks-- == 0))
    {
    net_state_enter(net_timeout_goto);
    }
  else
    {
    net_state_ticker1();
    }
  if ((net_granular_tick % 30)==0)    net_state_ticker30();
  if ((net_granular_tick % 60)==0)    net_state_ticker60();
  if ((net_granular_tick % 300)==0)   net_state_ticker300();
  if ((net_granular_tick % 600)==0)   net_state_ticker600();
  if ((net_granular_tick % 3600)==0)
    {
    net_state_ticker3600();
    net_granular_tick -= 3600;
    }

  if (net_state != NET_STATE_DIAGMODE)
    {
    if (--net_timeout_rxdata == 0)
      {
      // A major problem - we've lost connectivity to the modem
      // Possible cause: modem shutdown due to overheating / over current / ...
      // Best solution is to try switching on the modem and start over
      net_timeout_rxdata = NET_RXDATA_TIMEOUT;
      CHECKPOINT(0xF1)
      net_state_enter(NET_STATE_START);
      net_timeout_ticks = 11; // next stop: modem pwrkey
      }
    }
  }

////////////////////////////////////////////////////////////////////////
// net_ticker10th()
// This function is an entry point from the main() program loop, and
// gives the NET framework a ticker call approximately ten times per
// second. It is used to flash the RED LED when the link is up
//
void net_ticker10th(void)
  {
  }

////////////////////////////////////////////////////////////////////////
// net_initialise()
// This function is an entry point from the main() program loop, and
// gives the NET framework an opportunity to initialise itself.
//
void net_initialise(void)
  {
  UARTIntInit();

  net_reg = 0;
  net_state_enter(NET_STATE_FIRSTRUN);
  }


////////////////////////////////////////////////////////////////////////
// net_prep_alert()
// Prepare alert message in buffer for SMS/MSG
//
char *net_prep_alert(char *s, alert_type alert)
  {
  switch (alert)
    {
    case ALERT_SOCLOW:
      s = stp_i(net_scratchpad, "ALERT!!! CRITICAL SOC LEVEL APPROACHED (", car_SOC); // 95%
      s = stp_rom(s, "% SOC)");
      break;
      
    case ALERT_12VLOW:
      if (can_minSOCnotified & CAN_MINSOC_ALERT_12V)
        s = stp_l2f(s, "ALERT!!! 12V BATTERY CRITICAL (", car_12vline, 1);
      else
        s = stp_l2f(s, "12V BATTERY OK (", car_12vline, 1);
      s = stp_l2f(s, "V, ref=", car_12vline_ref, 1);
      s = stp_rom(s, "V)");
      break;
      
#ifndef OVMS_NO_VEHICLE_ALERTS
    case ALERT_TRUNK:
      s = stp_rom(s, "Trunk has been opened (valet mode).");
      break;
      
    case ALERT_ALARM:
      s = stp_rom(s, "Vehicle alarm is sounding!");
      break;
#endif
      
    case ALERT_CARON:
      s = stp_rom(s, "Vehicle is stopped turned on");
      break;
    }
  
  return s;
  }


