/*
 * Copyright (C) 2018 by Erik Hofman.
 * Copyright (C) 2018 by Adalin B.V.
 *
 * This file is part of AeonWave
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef AEONWAVE_INSTRUMENT_HPP
#define AEONWAVE_INSTRUMENT_HPP 1

#include <map>

#include <aax/aeonwave.hpp> 

namespace aax
{

class Note : public Emitter
{
private:
    Note(const Note&) = delete;

    Note& operator=(const Note&) = delete;

public:
    Note(float p) : Emitter(AAX_STEREO) {
        pitch_param = pitch = p;
        tie(pitch_param, AAX_PITCH_EFFECT, AAX_PITCH);
        tie(gain_param, AAX_VOLUME_FILTER, AAX_GAIN);
    }

    friend void swap(Note& n1, Note& n2) noexcept {
        std::swap(static_cast<Emitter&>(n1), static_cast<Emitter&>(n2));
        n1.pitch_param = std::move(n2.pitch_param);
        n1.gain_param = std::move(n2.gain_param);
        n1.pitch = std::move(n2.pitch);
        n1.pressure = std::move(n2.pressure);
        n2.playing = std::move(n2.playing);
    }

    Note& operator=(Note&&) = default;

    operator Emitter&() {
        return *this;
    }

    bool play(float gain) {
        gain_param = gain;
        Emitter::set(AAX_INITIALIZED);
        if (!playing) playing = Emitter::set(AAX_PLAYING);
        return playing;
    }

    bool stop() {
        playing = false;
        return Emitter::set(AAX_STOPPED);
    }

    bool buffer(Buffer& buffer) {
        Emitter::remove_buffer();
        return Emitter::add(buffer);
    }

    inline void set_pitch(float bend) { pitch_param = bend*pitch; }
    inline void set_pressure(uint8_t p) { pressure = p; }

private:
    Param pitch_param = 1.0f;
    Param gain_param = 1.0f;
    float pitch = 1.0f;
    uint8_t pressure = 0;
    bool playing = false;
};


class Instrument : public Mixer
{
private:
    Instrument(const Instrument& i) = delete;

    Instrument& operator=(const Instrument&) = delete;

public:
    Instrument(AeonWave& ptr) : Mixer(ptr), aax(&ptr) {
        Mixer::set(AAX_PLAYING);
    }

    friend void swap(Instrument& i1, Instrument& i2) noexcept {
        i1.key = std::move(i2.key);
        i1.aax = std::move(i2.aax);
        i1.pressure = std::move(i2.pressure);
        i1.playing = std::move(i2.playing);
    }

    Instrument& operator=(Instrument&&) = default;

    operator Mixer&() {
        return *this;
    }

    void play(uint8_t key_no, uint8_t velocity, Buffer& buffer, bool is_drums = false) {
        auto it = key.find(key_no);
        if (it == key.end()) {
            float pitch = 1.0f;
            float frequency = buffer.get(AAX_UPDATE_RATE);
            if (!is_drums) pitch = note2freq(key_no)/(float)frequency;
            auto ret = key.insert({key_no, new Note(pitch)});
            it = ret.first;
            Mixer::add(*it->second);
            if (!playing && !is_drums) {
                Mixer::add(buffer);
                playing = true;
            }
            it->second->buffer(buffer);
        }
        float gain = sqrtf((1+velocity)/128.0f);
        it->second->play(gain);
    }

    void stop(uint8_t key_no) {
        if (hold == false) {
            auto it = key.find(key_no);
            if (it != key.end()) {
                it->second->stop();
            }
        }
    }

    inline void set_pitch(float pitch) {
        for (auto& it : key) {
            it.second->set_pitch(pitch);
        }
    }

    inline void set_pressure(uint8_t p) { pressure = p; }

    void set_hold(bool sustain) {
        hold = sustain;
        if (!hold) {
            for (auto& it : key) {
                it.second->stop();
            }
        }
    }

    void set_pressure(uint8_t key_no, uint8_t pressure) {
        auto it = key.find(key_no);
        if (it != key.end()) {
            it->second->set_pressure(pressure);
        }
    }

private:
    inline float note2freq(uint8_t d) {
        return 440.0f*powf(2.0f, ((float)d-69.0f)/12.0f);
    }

    std::map<uint8_t,Note*> key;
    AeonWave* aax;

    uint8_t pressure = 0;
    bool playing = false;
    bool hold = false;
};

} // namespace aax

#endif

