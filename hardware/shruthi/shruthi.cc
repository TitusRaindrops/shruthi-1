// Copyright 2009 Olivier Gillet.
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "hardware/hal/adc.h"
#include "hardware/hal/audio_output.h"
#include "hardware/hal/devices/output_array.h"
#include "hardware/hal/devices/rotary_encoder.h"
#include "hardware/hal/devices/shift_register.h"
#include "hardware/hal/gpio.h"
#include "hardware/hal/init_atmega.h"
#include "hardware/hal/input_array.h"
#include "hardware/hal/serial.h"
#include "hardware/hal/time.h"
#include "hardware/hal/timer.h"
#include "hardware/midi/midi.h"
#include "hardware/shruthi/display.h"
#include "hardware/shruthi/editor.h"
#include "hardware/shruthi/synthesis_engine.h"
#include "hardware/utils/pretty_printer.h"
#include "hardware/utils/task.h"

using namespace hardware_hal;
using namespace hardware_midi;
using namespace hardware_shruthi;

using hardware_utils::NaiveScheduler;
using hardware_utils::Task;
using hardware_utils::PrettyPrinter;
using hardware_utils::endl;

// Midi input.
Serial<SerialPort1, 31250, BUFFERED, POLLED> midi_io;

// Input event handlers.
typedef MuxedAnalogInput<kPinAnalogInput> PotsMux;
typedef InputArray<PotsMux, kNumEditingPots, 8> Pots;

typedef InputArray<
    DigitalInput<kPinDigitalInput>,
    kNumGroupSwitches> Switches;

PwmOutput<kPinVcfCutoffOut> vcf_cutoff_out;
PwmOutput<kPinVcfResonanceOut> vcf_resonance_out;
PwmOutput<kPinVcaOut> vca_out;

Pots pots;
Switches switches;

// LED array.
OutputArray<
    Gpio<kPinLatch>, 
    Gpio<kPinClk>,
    Gpio<kPinData>, kNumLeds + 3, 4, MSB_FIRST, false> leds;

RotaryEncoder<
    Gpio<kPinEncoderA>,
    Gpio<kPinEncoderB>,
    Gpio<kPinEncoderClick> > encoder;

AudioOutput<PwmOutput<kPinVcoOut>, kAudioBufferSize, kAudioBlockSize> audio_out;

MidiStreamParser<SynthesisEngine> midi_parser;

typedef Serial<SerialPort0, 9600, DISABLED, POLLED> Debug;
PrettyPrinter<Debug> debug_output;

// What follows is a list of "tasks" - short functions handling a particular
// aspect of the synth (rendering audio, updating the LCD display, etc). they
// are called in sequence, with some tasks being more frequently called than
// others, by the Scheduler.
void UpdateLedsTask() {
  // Since the LCD and the shift register are sharing some lines, make sure that
  // the LCD is not currently writing some data before doing some stuff with
  // the shift register.
  leds.Clear();
  if (editor.current_page() == PAGE_MOD_MATRIX) {
    uint8_t current_modulation_source_value = engine.modulation_source(0,
        engine.patch().modulation_matrix.modulation[
            editor.subpage()].source);
    leds.set_value(LED_MOD_1, current_modulation_source_value >> 4);
    // TODO(pichenettes): write modulation destination too to LED2.
    // TODO(pichenettes): broken?
  } else if (editor.current_page() == PAGE_PERFORMANCE) {
    leds.set_value(engine.voice_controller().step() & 0x07, 15);
  } else {
    uint8_t pattern = editor.leds_pattern();
    uint8_t mask = 1;
    for (uint8_t i = 0; i < 8; ++i) {
      if (pattern & mask) {
        leds.set_value(i, 15);
      }
      mask <<= 1;
    }
  }
  // The led of the arpeggiator page flashes strongly on the 0-th step and
  // weakly on the other steps which are a multiple of 4.
  if (engine.voice_controller().active() &&
      editor.current_page() != PAGE_PERFORMANCE) {
    if (!(engine.voice_controller().step() & 3)) {
      leds.set_value(LED_PLAY, engine.voice_controller().step() ? 1 : 15);
    }
  }
  uint8_t muxed = switches.active_input();
  leds.set_value(8, muxed & 1 ? 15 : 0);
  leds.set_value(9, muxed & 2 ? 15 : 0);
  leds.set_value(10, muxed & 4 ? 15 : 0);
  leds.Output();
}

void UpdateDisplayTask() {
  display.Tick();
}

void InputTask() {
  Switches::Event switch_event;
  Pots::Event pot_event;
  static uint8_t idle;
  static uint8_t target_page_type;
  static int8_t delta;
TASK_BEGIN_NEAR
  while (1) {
    idle = 0;
    target_page_type = PAGE_TYPE_ANY;
    
    // Read the switches.
    switch_event = switches.Read();
    
    // If a button was pressed, perform the action. Otherwise, if nothing
    // happened for 1.5s, update the idle flag.
    if (switch_event.event == EVENT_NONE) {
      if (switch_event.time > 1500) {
        idle = 1;
      }
    } else {
      if (switch_event.event == EVENT_RAISED && switch_event.time > 100) {
        uint8_t id = switch_event.id;
        uint8_t hold_time = static_cast<uint16_t>(switch_event.time) >> 8;
        if (hold_time >= 3) {  // 0.768 seconds
          editor.DoShiftFunction(id, hold_time);
        } else {
          editor.ToggleGroup(id);
        }
        target_page_type = PAGE_TYPE_SUMMARY;
      }
    }
    
    // Select which analog/digital inputs we want to read by a write to the
    // multiplexer register.
    pot_event = pots.Read();
    PotsMux::set_pin(pots.active_input());
    
    // Update the editor if something happened.
    // Revert back to the main page when nothing happened for 1.5s.
    if (pot_event.event == EVENT_NONE) {
      if (idle && pot_event.time > 1500) {
        target_page_type = PAGE_TYPE_SUMMARY;
      }
    } else {
      editor.HandleInput(pot_event.id, pot_event.value);
      target_page_type = PAGE_TYPE_DETAILS;
    }
    TASK_SWITCH;
    
    delta = encoder.Read();
    if (delta) {
      // Simulates a press on a switch to restart page detail timer.
      switches.Touch();  
      editor.HandleIncrement(delta);
      target_page_type = PAGE_TYPE_DETAILS;
    }
    TASK_SWITCH;
    
#ifdef HAS_EASTER_EGG
    if (engine.zobi() == 18) {
      editor.DisplaySplashScreen(STR_RES_P_ORLEANS_21_MN);
    } else
#endif  // HAS_EASTER_EGG
    if (target_page_type == PAGE_TYPE_SUMMARY) {
      editor.DisplaySummary();
    } else if (target_page_type == PAGE_TYPE_DETAILS) {
      editor.DisplayDetails();
    }
    TASK_SWITCH;
  }
TASK_END
}

uint8_t current_cv;

void CvTask() {
  ++current_cv;
  if (current_cv >= kNumCvInputs) {
    current_cv = 0;
  }
  engine.set_cv(current_cv, Adc::Read(kPinCvInput + current_cv) >> 2);
}

void MidiTask() {
  if (midi_io.readable()) {
    uint8_t value = midi_io.ImmediateRead();
    
    // Copy the byte to the MIDI output (thru). We could use Overwrite here
    // since the output rate is the same as the input rate, which ensures that
    // 0.32ms have elapsed between the writes.
    midi_io.Write(value);
    
    // Also, parse the message.
    uint8_t status = midi_parser.PushByte(value);
    
    // Display a status indicator on the LCD to indicate that a message has
    // been received. This could be done as well in the synthesis engine code
    // or in the MIDI parser, but I'd rather keep the UI code separate.
    switch (status & 0xf0) {
      // Note on/off.
      case 0x90:
        display.set_status('\x01');
        break;
      // Controller.
      case 0xb0:
        display.set_status('\x05');
        break;
      // Bender.
      case 0xe0:
        display.set_status('\x02');
        break;
      // Special messages.
      case 0xf0:
        // Display a status indicator to monitor SysEx patch reception.
        if (status == 0xf0 || status == 0xf7) {
          switch (engine.patch().sysex_reception_state()) {
            case RECEIVING_DATA:
              display.set_status('~');
              break;
            case RECEPTION_OK:
              display.set_status('+');
              engine.TouchPatch();
              break;
            case RECEPTION_ERROR:
              display.set_status('#');
              break;
          }
        }
        break;
    }
  }
}

void AudioRenderingTask() {
  if (audio_out.writable_block()) {
    engine.Control();
    for (uint8_t i = kAudioBlockSize; i > 0 ; --i) {
      engine.Audio();
      audio_out.Overwrite(engine.voice(0).signal());
    }
    
    vcf_cutoff_out.Write(engine.voice(0).cutoff());
    vcf_resonance_out.Write(engine.voice(0).resonance());
    vca_out.Write(engine.voice(0).vca());
  }
}

uint16_t previous_num_glitches;

// This task displays a '!' in the status area of the LCD displays whenever
// a discontinuity occurred in the audio rendering. Even if the code is
// optimized in such a way that it never occurs, I'd rather keep it here in
// case new features are implemented and need performance monitoring.
// This code uses 42 bytes.
void AudioGlitchMonitoringTask() {
  uint16_t num_glitches = audio_out.num_glitches();
  if (num_glitches != previous_num_glitches) {
    previous_num_glitches = num_glitches;
    display.set_status('!');
  }
}

typedef NaiveScheduler<kSchedulerNumSlots> Scheduler;

Scheduler scheduler;

/* static */
template<>
Task Scheduler::tasks_[] = {
    { &AudioRenderingTask, 16 },
    { &MidiTask, 6 },
    { &UpdateLedsTask, 4 },
    { &UpdateDisplayTask, 2 },
    { &AudioGlitchMonitoringTask, 1 },
    { &InputTask, 2 },
    { &CvTask, 1 },
};

uint8_t lcd_write_cycle = 0;

TIMER_2_TICK {
  audio_out.EmitSample();
  lcd_write_cycle = (lcd_write_cycle + 1) & 0x0f;
  if (lcd_write_cycle == 0) {
    lcd.Tick();
  }
}

void Init() {
  Debug::Init();
  
  scheduler.Init();
  
  lcd.Init();
  display.Init();
  lcd.SetCustomCharMapRes(character_table[0], 8, 0);
  
  editor.Init();
  audio_out.Init();

  // Initialize all the PWM outputs, in 31.25kHz, phase correct mode.
  Timer<1>::set_prescaler(1);
  Timer<1>::set_mode(TIMER_PWM_PHASE_CORRECT);
  Timer<2>::set_prescaler(1);
  Timer<2>::set_mode(TIMER_PWM_PHASE_CORRECT);
  Timer<2>::Start();
  
  vcf_cutoff_out.Init();
  vcf_resonance_out.Init();
  vca_out.Init();
  
  editor.DisplaySplashScreen(STR_RES_MUTABLE);
  
  midi_io.Init();
  pots.Init();
  switches.Init();
  encoder.Init();
  DigitalInput<kPinDigitalInput>::EnablePullUpResistor();
  leds.Init();  
  
  engine.Init();
}

int main(void) {
  InitAtmega(false);  // Do not initialize timers 1 & 2.
  Init();
  scheduler.Run();
}
