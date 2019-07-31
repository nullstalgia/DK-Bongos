/* Copyright (c) 2014-2016 NicoHood

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE. */
#include "bongos.h"

uint8_t gcTransceive(const uint8_t pin, uint8_t* command, const uint8_t cmdLen, uint8_t* buffer, const uint8_t buffLen) {
    uint8_t bitMask = digitalPinToBitMask(pin);
    uint8_t port = digitalPinToPort(pin);
    volatile uint8_t* modePort = portModeRegister(port);
    volatile uint8_t* outPort = portOutputRegister(port);
    volatile uint8_t* inPort = portInputRegister(port);

    uint8_t oldSREG = SREG;
    cli();

    transmit(command, cmdLen, modePort, outPort, bitMask);
    uint8_t temp = receive(buffer, buffLen, modePort, outPort, inPort, bitMask);

    SREG = oldSREG;
    return temp;
}

// TODO integrate this macro function with the nop_block() macro
#define nopManual(n) nopn ## n // n % 3 nops
#define nopn1 "nop\n"
#define nopn2 "nop\nnop\n"
#define nopn4 nopn1
#define nopn8 nopn2
#define nopn10 nopn1
#define nopn12
#define nopn14 nopn2
#define nopn30
#define nopn43 nopn1
/* only 3 or less nops don't get optimized out guarenteed https://www.avrfreaks.net/forum/problems-delaying-nops-avr-gcc
(1) ldi + floor(N - 1 / 3) * ((1) dec + (2) brne passes) + (1) dec + (1) brne fails + N % 3 = N nops */
#define nop_block(id, N) "ldi %[nop], (" #N "/3)\n.L%=_nop_loop" #id ":\ndec %[nop]\nbrne .L%=_nop_loop" #id "\n" nopManual(N)

// TODO simplify assembly

void gcTransmit(const uint8_t* buff, uint8_t len, volatile uint8_t* modePort, volatile uint8_t* outPort, uint8_t bitMask) {
    *outPort |= bitMask; // high b/c idle high
    *modePort |= bitMask; // output

    // clobbers
    register uint8_t bitCount;
    register uint8_t data;
    register uint8_t nop;

    asm volatile (
        ";gcTransmit()\n"

        // passed in to this block are:
        // the %a[buff] register is the buffer pointer
        // %[len] is the register holding the length of the buffer in bytes

        // Instruction cycles are noted in parentheses
        // branch instructions have two values, one if the branch isn't
        // taken and one if it is

        // %[data] will be the current buffer byte loaded from memory
        // %[bitCount] will be the bit counter for the current byte. when this
        // reaches 0, we need to decrement the length counter, load
        // the next buffer byte, and loop. (if the length counter becomes
        // 0, that's our exit condition)

        // This label starts the outer loop, which sends a single byte
        ".L%=_byte_loop:\n"
        "ld %[data], %a[buff]+\n" // (2) load the next byte and increment byte pointer
        "ldi %[bitCount],0x08\n" // (1) set bitcount to 8 bits

        // This label starts the inner loop, which sends a single bit
        ".L%=_bit_loop:\n"
        "st %a[outPort],%[low]\n" // (2) pull the line low

        // line needs to stay low for 1�s for a 1 bit, 3�s for a 0 bit
        // this block figures out if the next bit is a 0 or a 1
        // the strategy here is to shift the register left, then test and
        // branch on the carry flag
        "lsl %[data]\n" // (1) shift left. MSB goes into carry bit of status reg
        "brcc .L%=_zero_bit\n" // (1/2) branch if carry is cleared


        // this block is the timing for a 1 bit (1�s low, 3�s high)
        // Stay low for 2uS: 16 - 2 (above lsl,brcc) - 2 (below st) = 12 cycles
        nop_block(1, 12) // nop block 1, 12 cycles

        "st %a[outPort],%[high]\n" // (2) set the line high again
        // Now stay high for 2�s of the 3�s to sync up with the branch below
        // 2*16 - 2 (for the rjmp) = 30 cycles
        nop_block(2, 30) // nop block 2, 30 cycles
        "rjmp .L%=_finish_bit\n" // (2)


        // this block is the timing for a 0 bit (3�s low, 1�s high)
        // Need to go high in 3*16 - 3 (above lsl,brcc) - 2 (below st) = 43 cycles
        ".L%=_zero_bit:\n"
        nop_block(3, 43) // nop block 3, 43 cycles
        "st %a[outPort],%[high]\n" // (2) set the line high again


        // The two branches meet up here.
        // We are now *exactly* 3�s into the sending of a bit, and the line
        // is high again. We have 1�s to do the looping and iteration
        // logic.
        ".L%=_finish_bit:\n"
        "dec %[bitCount]\n" // (1) subtract 1 from our bit counter
        "breq .L%=_load_next_byte\n" // (1/2) branch if we've sent all the bits of this byte

        // At this point, we have more bits to send in this byte, but the
        // line must remain high for another 1�s (minus the above
        // instructions and the jump below and the st instruction at the
        // top of the loop)
        // 16 - 2(above) - 2 (rjmp below) - 2 (st after jump) = 10
        nop_block(4, 10) // nop block 4, 10 cycles
        "rjmp .L%=_bit_loop\n" // (2)


        // This block starts 3 cycles into the last 1�s of the line being high
        // We need to decrement the byte counter. If it's 0, that's our exit condition.
        // If not we need to load the next byte and go to the top of the byte loop
        ".L%=_load_next_byte:\n"
        "dec %[len]\n" // (1) len--
        "breq .L%=_loop_exit\n" // (1/2) if the byte counter is 0, exit
        // delay block:
        // needs to go high after 1�s or 16 cycles
        // 16 - 5 (above) - 2 (the jump itself) - 5 (after jump) = 4
        nop_block(5, 4) // nop block 5, 4 cycles
        "rjmp .L%=_byte_loop\n" // (2)


        // Loop exit
        ".L%=_loop_exit:\n"

        // final task: send the stop bit, which is a 1 (1�s low 3�s high)
        // the line goes low in:
        // 16 - 6 (above since line went high) - 2 (st instruction below) = 8 cycles
        nop_block(6, 8) // nop block 6, 8 cycles
        "st %a[outPort],%[low]\n" // (2) pull the line low
        // stay low for 1�s
        // 16 - 2 (below st) = 14
        nop_block(7, 14) // nop block 7, 14 cycles
        "st %a[outPort],%[high]\n" // (2) set the line high again
        // just stay high. no need to wait 3�s before returning

        // ----------
        // outputs:
        : [buff] "+e" (buff), // (read and write)
        [outPort] "+e" (outPort), // (read and write)
        [bitCount] "=&d" (bitCount), // (output only, ldi needs the upper registers)
        [data] "=&r" (data), // (output only)
        [nop] "=&d" (nop) // (output only, ldi needs the upper registers)

        // inputs:
        : [len] "r" (len),
        [high] "r" (*outPort | bitMask), // precalculate new pin states
        [low] "r" (*outPort & ~bitMask) // this works because we turn interrupts off

        // no clobbers
    );
}

uint8_t gcReceive(uint8_t* buff, uint8_t len, volatile uint8_t* modePort, volatile uint8_t* outPort, volatile uint8_t * inPort, uint8_t bitMask) {
    *modePort &= ~bitMask; // input w/pullup
    *outPort |= bitMask;

    // clobbers
    register uint8_t timeoutCount; // counts down the timeout
    register uint8_t bitCount; // counts down 8 bits for each byte
    register uint8_t inputVal; // temporary variable to save the pin states
    register uint8_t data; // keeps the temporary received data byte
    register uint8_t receivedBytes; // the return value of the function
    register uint8_t initialTimeoutCount; // extra timeout count for initial function call

    asm volatile (
        ";gcReceive()\n"

        // [bitCount] is our bit counter. We read %[len] bytes
        // and increment the byte pointer and receivedBytes every 8 bits
        "ldi %[bitCount],0x08\n" // (1) set bitcount to 8 bits
        "ldi %[receivedBytes],0x00\n" // (1) default exit value is 0 bytes received

        // Initial loop waits for the line to go low.
        // This is especially required when reading the console commands.
        // When reading controller commands the normal timeout below is fine.
        // The gamecube will poll for an init command every 66ms if not connected.
        // If a controller is connected it will be polled every X ms.
        // 8ms (Smash Melee), 0.7 - 9ms (GC options), 0.6 - 19 ms (Mario Football)
        // This function will wait for 71.22ms to pull the line low.
        // This way we ensure to catch at least a single gamecube command if connected.
        "ldi %[initialTimeoutCount],0x00\n" // (1) set the outer timeout

        // Inititally try to read more often to get a longer timeout.
        // This is the first (of two) extre initital loops.
        ".L%=_wait_for_low_extra:\n"
        "ldi %[timeoutCount],0x00\n" // (1) set the extra timeout
        ".L%=_wait_for_low_loop_extra1:\n" // 7 cycles if loop fails
        "ld %[inputVal], %a[inPort]\n" // (2) read the pin (happens before the 2 cycles)
        "and %[inputVal], %[bitMask]\n" // (1) compare pinstate with bitmask
        "breq .L%=_wait_for_measure\n" // (1/2) jump to the measure part if pin is low
        // the following happens if the line is still high
        "dec %[timeoutCount]\n" // (1) decrease timeout by 1
        "brne .L%=_wait_for_low_loop_extra1\n" // (1/2) loop if the counter isn't 0

        // 2n initial extra loop
        "ldi %[timeoutCount],0x00\n" // (1) set the extra timeout
        ".L%=_wait_for_low_loop_extra2:\n" // 7 cycles if loop fails
        "ld %[inputVal], %a[inPort]\n" // (2) read the pin (happens before the 2 cycles)
        "and %[inputVal], %[bitMask]\n" // (1) compare pinstate with bitmask
        "breq .L%=_wait_for_measure\n" // (1/2) jump to the measure part if pin is low
        // the following happens if the line is still high
        "dec %[timeoutCount]\n" // (1) decrease timeout by 1
        "brne .L%=_wait_for_low_loop_extra2\n" // (1/2) loop if the counter isn't 0


        // This first spinloop waits for the line to go low.
        // It loops 128 times before it gives up and returns.
        // This timeout is used after the initital timeout and times out a lot faster.
        // For reading a Gamecube controller this loop timeout would be fine without the outer loop.
        ".L%=_wait_for_low:\n"
        "ldi %[timeoutCount],%[timeout]\n" // (1) set the timeout
        ".L%=_wait_for_low_loop:\n" // 7 cycles if loop fails
        "ld %[inputVal], %a[inPort]\n" // (2) read the pin (happens before the 2 cycles)
        "and %[inputVal], %[bitMask]\n" // (1) compare pinstate with bitmask
        "breq .L%=_wait_for_measure\n" // (1/2) jump to the measure part if pin is low
        // The following happens if the line is still high
        "dec %[timeoutCount]\n" // (1) decrease timeout by 1
        "brne .L%=_wait_for_low_loop\n" // (1/2) loop if the counter isn't 0


        // Wait for line to initially go low with the outer loop.
        // If one bit was already read initialTimeoutCount will be set to 1 to disable the outer loop.
        // Calculate delay: (((7 × 255) * 2 + (7 × 128) + 3) × 255) ÷ 16000 = 71,22ms > 66ms
        "dec %[initialTimeoutCount]\n" // (1) decrease outer timeout by 1
        "brne .L%=_wait_for_low_extra\n" // (1/2) loop if the counter isn't 0
        "rjmp .L%=_exit\n" // (2) timeout, jump to the end


        // Next block. The line has just gone low. Wait approx 2�s
        // each cycle is 1/16 �s on a 16Mhz processor
        // best case: 32 - 5 (above) - 1 (below) = 26 nops
        // worst case: 32 - 5 (above) - 6 (above, worst case) - 1 (below) = 20 nops
        // --> 23 nops
        ".L%=_wait_for_measure:\n"
        // nop block, 23 cycles, use inputVal as temporary reg since we dont need it right now
        "ldi %[inputVal], (23/3)\n" /* (1) ldi, start */
        ".L%=_nop_loop1:\n" /* + ((N-1) * (1) dec + (2) brne), (N-1) loops */
        "dec %[inputVal]\n" /* + (1) dec + (1) brne, last loop */
        "brne .L%=_nop_loop1\n" /* --> (N * 3) nops */
        nopManual(1) /* 23 % 3 = 2 manual nops minus the outer timeout disabling below */
        "ldi %[initialTimeoutCount],1\n" // (1) disable the outer timeout

        // save the data
        "lsl %[data]\n" // (1) left shift the current byte in %[data]
        "ld %[inputVal], %a[inPort]\n" // (2) read the pin (happens before the 2 cycles)
        "and %[inputVal], %[bitMask]\n" // (1) compare pinstate with bitmask
        "breq .L%=_check_bit_count\n" // (1/2) skip setting data to 1 if pin is low
        "sbr %[data],0x01\n" // set bit 1 in %[data] if pin is high
        ".L%=_check_bit_count:\n"
        "dec %[bitCount]\n" // (1) decrement 1 from our bit counter
        "brne .L%=_wait_for_high\n" // (1/2) branch if we've not received the whole byte

        // we received a full byte
        "st %a[buff]+,%[data]\n" // (2) save %[data] back to memory and increment byte pointer
        "inc %[receivedBytes]\n" // (1) increase byte count
        "ldi %[bitCount],0x08\n" // (1) set bitcount to 8 bits again
        "cp %[len],%[receivedBytes]\n" // (1) %[len] == %[receivedBytes] ?
        "breq .L%=_exit\n" // (1/2) jump to exit if we received all bytes
        // dont wait for line to go high again


        // This next block waits for the line to go high again.
        // again, it sets a timeout counter of 128 iterations
        ".L%=_wait_for_high:\n"
        "ldi %[timeoutCount],%[timeout]\n" // (1) set the timeout
        ".L%=_wait_for_high_loop:\n" // 7 cycles if loop fails
        "ld %[inputVal], %a[inPort]\n" // (2) read the pin (happens before the 2 cycles)
        "and %[inputVal], %[bitMask]\n" // (1) compare pinstate with bitmask
        "brne .L%=_wait_for_low\n" // (1/2) line is high. ready for next loop
        // the following happens if the line is still low
        "dec %[timeoutCount]\n" // (1) decrease timeout by 1
        "brne .L%=_wait_for_high_loop\n" // (1/2) loop if the counter isn't 0
        // timeout, exit now
        ".L%=_exit:\n"

        // ----------
        // outputs:
        : [receivedBytes] "=&d" (receivedBytes), // (ldi needs the upper registers)
        [buff] "+e" (buff), // (read and write)
        [bitCount] "=&d" (bitCount), // (output only, ldi needs the upper registers)
        [timeoutCount] "=&r" (timeoutCount), // (output only)
        [initialTimeoutCount] "=&r" (initialTimeoutCount), // (output only)
        [inputVal] "=&r" (inputVal), // (output only)
        [data] "=&r" (data) // (output only)

        // inputs
        : [len] "r" (len),
        [inPort] "e" (inPort),
        [bitMask] "r" (bitMask),
        [timeout] "M" (128) // constant
    );

    return receivedBytes;
}


bool gccInitialize(const uint8_t pin, GCStatus_t* stat) {
    uint8_t command[] = { 0x00 };
    return gcTransceive(pin, command, sizeof(command), (uint8_t*)stat, sizeof(stat)) == sizeof(stat);
}

bool gccOriginate(const uint8_t pin, GCOrigin_t* orig) {
    uint8_t command[] = { 0x41 };
    return gcTransceive(pin, command, sizeof(command), (uint8_t*)orig, sizeof(orig)) == sizeof(orig);
}

bool gccPoll(const uint8_t pin, GCReport_t* rept, const bool rumble) {
    uint8_t command[] = { 0x40, 0x03, rumble };
    return gcTransceive(pin, command, sizeof(command), (uint8_t*)rept, sizeof(rept)) == sizeof(rept);
}

uint8_t gcWrite(const uint8_t pin, GCStatus_t* stat, GCOrigin_t* orig, GCReport_t* rept) {
    uint8_t ret = 0;
    uint8_t bitMask = digitalPinToBitMask(pin);
    uint8_t port = digitalPinToPort(pin);
    volatile uint8_t* modePort = portModeRegister(port);
    volatile uint8_t* outPort = portOutputRegister(port);
    volatile uint8_t* inPort = portInputRegister(port);

    uint8_t oldSREG = SREG;
    cli();

    uint8_t command[3] = { 0, 0, 0 };
    uint8_t numRecv = receive(command, sizeof(command), modePort, outPort, inPort, bitMask);

    if (numRecv == 1) {
        if (command[0] == 0x00) {
            transmit(stat->raw, sizeof(stat), modePort, outPort, bitMask);
            ret = 1;
        } else if (command[0] == 0x41) {
            transmit(orig->raw, sizeof(orig), modePort, outPort, bitMask);
            ret = 2;
        }
    } else if (numRecv == 3 && command[0] == 0x40 && command[1] == 0x03) {
        transmit(rept->raw, sizeof(rept), modePort, outPort, bitMask);
        ret = 3 + command[2] % 4;
    }

    SREG = oldSREG;
    return ret; // 1 = init, 2 = origin, 3-6 = read w/rumble state
}
