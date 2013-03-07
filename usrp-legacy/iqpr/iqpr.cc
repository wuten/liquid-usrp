/*
 * Copyright (c) 2010, 2011, 2012 Joseph Gaeddert
 * Copyright (c) 2010, 2011, 2012 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// iqpr.cc
//
// iqpr: l(iq)uid (p)acket (r)adio
//

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex>

#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#include <liquid/liquid.h>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include "iqpr.h"

#define IQPR_VERBOSE 0

void * iqpr_rx_process(void * _arg);

// iqpr data structure
struct iqpr_s {
    // UHD interface to hardware
    uhd::usrp::multi_usrp::sptr usrp;

    //
    unsigned int M;                 // number of subcarriers
    unsigned int cp_len;            // cyclic prefix length
    unsigned int taper_len;         // taper length
    unsigned char * p;              // subcarrier allocation

    // 
    // receiver
    //
    std::vector<std::complex<float> > * rx_buffer;    // rx data buffer
    unsigned int rx_vector_index;                   // index of rx buffer vector
    unsigned int rx_vector_length;                  // length of rx buffer vector
    resamp2_crcf rx_decim;          // half-band decimator
    resamp_crcf rx_resamp;          // arbitrary resampler
    ofdmflexframesync fs;               // frame synchronizer
    int rx_packet_found;            // receiver packet found flag
    unsigned char rx_header[8];    // received frame header
    int rx_header_valid;            // receiver frame header valid?
    unsigned char * rx_payload;        // received payload data
    unsigned int rx_payload_len;       // received payload data length
    unsigned int rx_payload_numalloc;  // received payload data length (num allocated)
    int rx_payload_valid;           // receiver payload data valid?
    framesyncstats_s rx_stats;      // frame synchronizer stats

    // receiver control (threading)
    int rx_running;                 // receiver running flag
    int rx_waiting;                 // receiver waiting for packet flag
    pthread_t rx_process;           // receiver thread
    pthread_mutex_t rx_mutex;       // 
    pthread_cond_t rx_cond;         // 

    // 
    // transmitter
    //
    unsigned char * tx_data;        // transmitted payload data
    unsigned int tx_data_len;       // transmitted payload data length
    ofdmflexframegenprops_s fgprops;    // frame generator properties
    ofdmflexframegen fg;                // frame generator
    resamp2_crcf tx_interp;         // half-band interpolator
    resamp_crcf tx_resamp;          // arbitrary resampler
    //std::complex<float> * frame;    // frame buffer
    std::complex<float> data_tx[32];                // tx data buffer (array)
    std::complex<float> data_tx_interp[128];        // tx interpolated
    std::complex<float> data_tx_resamp[256];        // tx resampled
    std::vector<std::complex<float> > tx_buffer;    // tx data buffer

    // debugging
    int verbose;

    // 
    // higher-level functionality
    //
    unsigned int node_id;           // node identifier
};


// create iqpr object
iqpr iqpr_create()
{
    // allocate memory for main object
    iqpr q = (iqpr) malloc(sizeof(struct iqpr_s));

    // TODO : create USRP object
    uhd::device_addr_t dev_addr;
    // TODO : set up address as necessary
    q->usrp = uhd::usrp::multi_usrp::make(dev_addr);

    // set some properties
    q->usrp->set_rx_antenna("TX/RX");
    //q->usrp->set_rx_antenna("RX2");
    // some hacked magic to 'disable' receive chain when transmitting
    // (from http://www.ruby-forum.com/topic/1527488)
    //q->usrp->set_rx_lo_freq((_freq_range.start() + _freq_range.stop())/2.0);

    //
    // common
    //
    q->M = 64;          // number of subcarriers
    q->cp_len = 8;      // cyclic prefix length
    q->taper_len = 4;   // taper length
    q->p = NULL;        // subcarrier allocation (NULL gives default)
    q->p = (unsigned char*)malloc(q->M*sizeof(unsigned char));
    unsigned int guard = q->M / 6;
    unsigned int pilot_spacing = 8;
    unsigned int i0 = (q->M/2) - guard;
    unsigned int i1 = (q->M/2) + guard;
    unsigned int i;
    for (i=0; i<q->M; i++) {
        if ( i == 0 || (i > i0 && i < i1) )
            q->p[i] = OFDMFRAME_SCTYPE_NULL;
        else if ( (i%pilot_spacing)==0 )
            q->p[i] = OFDMFRAME_SCTYPE_PILOT;
        else
            q->p[i] = OFDMFRAME_SCTYPE_DATA;
    }

    // 
    // receiver objects
    //
    const size_t max_samps_per_packet = q->usrp->get_device()->get_max_recv_samps_per_packet();
    q->rx_buffer = new std::vector< std::complex<float> >(max_samps_per_packet);
    printf("rx buffer size: %u\n", (unsigned int)(q->rx_buffer->size()));
    q->rx_vector_index  = 0;
    q->rx_vector_length = 0;
    q->rx_resamp = resamp_crcf_create(1.0, 7, 0.4, 60.0, 64);
    q->rx_decim = resamp2_crcf_create(7, 0.0, 40.0);

    // create frame synchronizer
    q->fs = ofdmflexframesync_create(q->M,
                                     q->cp_len,
                                     q->taper_len,
                                     q->p,
                                     iqpr_callback,
                                     (void*)q);

    // allocate memory for received data
    q->rx_payload_len      = 1024;
    q->rx_payload_numalloc = q->rx_payload_len;
    q->rx_payload = (unsigned char*) malloc(q->rx_payload_numalloc*sizeof(unsigned char));

    // initialize threading objects
    q->rx_running = 0;
    q->rx_waiting = 0;
    pthread_mutex_init(&q->rx_mutex, NULL);
    pthread_cond_init(&q->rx_cond, NULL);


    // 
    // transmitter objects
    //

    // create frame generator
    ofdmflexframegenprops_init_default(&q->fgprops);
    q->fgprops.check        = LIQUID_CRC_NONE;
    q->fgprops.fec0         = LIQUID_FEC_NONE;
    q->fgprops.fec1         = LIQUID_FEC_NONE;
    q->fgprops.mod_scheme   = LIQUID_MODEM_QPSK;
    q->fg = ofdmflexframegen_create(q->M,
                                    q->cp_len,
                                    q->taper_len,
                                    q->p,
                                    &q->fgprops);
    ofdmflexframegen_print(q->fg);

    // create interpolator
    q->tx_interp = resamp2_crcf_create(7,0.0f,40.0f);

    q->tx_resamp = resamp_crcf_create(1.0, 7, 0.4, 60.0, 64);

    //q->tx_buffer.resize(1);

    // set hardware transmit/receive gains
    iqpr_set_tx_gain(q, 40.0f);
    iqpr_set_rx_gain(q, 40.0f);

    // debugging
    q->verbose = 0;

    return q;
}

void iqpr_destroy(iqpr _q)
{
    // 
    // common objects
    //
    if (_q->p != NULL)
        free(_q->p);

    // destroy receiver buffer
    delete _q->rx_buffer;

    // 
    // receiver objects
    //
    resamp_crcf_destroy(_q->rx_resamp);
    resamp2_crcf_destroy(_q->rx_decim);
    ofdmflexframesync_destroy(_q->fs);
    free(_q->rx_payload);
    
    pthread_mutex_destroy(&_q->rx_mutex);
    pthread_cond_destroy(&_q->rx_cond);


    // 
    // transmitter objects
    //
    ofdmflexframegen_destroy(_q->fg);
    resamp2_crcf_destroy(_q->tx_interp);
    resamp_crcf_destroy(_q->tx_resamp);

    // free main object memory
    free(_q);
}

void iqpr_print(iqpr _q)
{
    printf("iqpr:\n");
}

// set verbosity on
void iqpr_set_verbose(iqpr _q)
{
    _q->verbose = 1;
}

// set verbosity off
void iqpr_unset_verbose(iqpr _q)
{
    _q->verbose = 0;
}

// set transmit/receive hardware gain
void iqpr_set_tx_gain(iqpr _q, float _tx_gain)
{
    _q->usrp->set_tx_gain(_tx_gain);
}

// set transmit/receive hardware gain
void iqpr_set_rx_gain(iqpr _q, float _rx_gain)
{
    _q->usrp->set_rx_gain(_rx_gain);
}

#if 0
// set transmit/receive software gain
void iqpr_set_tx_power(iqpr _q, float _tx_power);
void iqpr_set_rx_power(iqpr _q, float _rx_power);
#endif

// set transmit/receive sample rate
void iqpr_set_tx_rate(iqpr _q, float _tx_rate)
{
    unsigned long int DAC_RATE = 64e6;

    // over-sampling by a factor of 4
    _tx_rate *= 4.0;

    unsigned int interp_rate = (unsigned int)(DAC_RATE / _tx_rate);
    // ensure multiple of 4
    interp_rate = (interp_rate >> 2) << 2;
    
    //
    while (interp_rate == 240 || interp_rate == 244)
        interp_rate -= 4;

    // compute usrp sampling rate
    double usrp_tx_rate = DAC_RATE / (double)interp_rate;

    // set hardware sampling rate
    _q->usrp->set_tx_rate(usrp_tx_rate);

    // get actual rate
    usrp_tx_rate = _q->usrp->get_tx_rate();

    // compute arbitrary resampling rate
    double tx_resamp_rate = usrp_tx_rate / _tx_rate;
    
    // set software resampling rate
    resamp_crcf_setrate(_q->tx_resamp, tx_resamp_rate);

    printf("sample rate :   %12.8f kHz = %12.8f * %8.6f (interp %u)\n",
            _tx_rate * 1e-3f,
            usrp_tx_rate * 1e-3f,
            1.0f / tx_resamp_rate,
            interp_rate);
}

// set transmit/receive sample rate
void iqpr_set_rx_rate(iqpr _q, float _rx_rate)
{
    unsigned long int ADC_RATE = 64e6;

    // over-sampling by a factor of 4
    _rx_rate *= 4.0;

    unsigned int decim_rate = (unsigned int)(ADC_RATE / _rx_rate);
    // ensure multiple of 2
    decim_rate = (decim_rate >> 1) << 1;

    // compute usrp sampling rate
    double usrp_rx_rate = ADC_RATE / decim_rate;

    // set hardware sampling rate
    _q->usrp->set_rx_rate(usrp_rx_rate);

    // get actual hardware sampling rate
    usrp_rx_rate = _q->usrp->get_rx_rate();

    // compute arbitrary resampling rate
    double rx_resamp_rate = _rx_rate / usrp_rx_rate;

    // set software resampling rate
    resamp_crcf_setrate(_q->rx_resamp, rx_resamp_rate);

    printf("sample rate :   %12.8f kHz = %12.8f * %8.6f (decim %u)\n",
            _rx_rate * 1e-3f,
            usrp_rx_rate * 1e-3f,
            rx_resamp_rate,
            decim_rate);
}

// set transmit/receive frequency
void iqpr_set_tx_freq(iqpr _q, float _tx_freq)
{
    _q->usrp->set_tx_freq(_tx_freq);
}

// set transmit/receive frequency
void iqpr_set_rx_freq(iqpr _q, float _rx_freq)
{
    _q->usrp->set_rx_freq(_rx_freq);
}

// start receiver thread
void iqpr_rx_start(iqpr _q)
{
    // check if receiver is running
    if (_q->rx_running) {
        fprintf(stderr,"warning: iqpr_rx_start() invoked, but receiver is already running\n");
        return;
    }

    // create and start thread
#if IQPR_VERBOSE
    printf("iqpr_rx_start(), starting thread\n");
#endif
    _q->rx_running = 1;
    pthread_create(&_q->rx_process, NULL, iqpr_rx_process, (void*)_q);
}

// stop data transfer
void iqpr_rx_stop(iqpr _q)
{
    _q->rx_running = 0;

    // join threads (stop rx running)
    void * exit_status;
    printf("waiting for process to finish...\n");
    pthread_join(_q->rx_process, &exit_status);
} 


// start data transfer
void * iqpr_rx_process(void * _arg)
{
    // type cast input argument as iqpr object
    iqpr _q = (iqpr) _arg;

    printf("issuing stream command...\n");
    _q->usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    // clear object internals
    resamp2_crcf_clear(_q->rx_decim);
    resamp_crcf_reset(_q->rx_resamp);
    ofdmflexframesync_reset(_q->fs);
    
    // allocate temporary buffers
    std::complex<float> rx_buffer_decim[2];
    std::complex<float> rx_decim_out;
    std::complex<float> rx_buffer_resamp[4];


    _q->rx_vector_index  = 0;
    _q->rx_vector_length = 0;

    //return;

    // initialize metadata
    uhd::rx_metadata_t md;

    // chomp data
    //while ( num_accumulated_samples < total_samples) {
    // read samples from buffer, run through frame synchronizer
    unsigned int n=0;
    unsigned int nw=0;
    while ( _q->rx_running ) {
        // printf("still running...\n");

        // check if we have read entire contents of buffer
        while (_q->rx_vector_index == _q->rx_vector_length) {
            // reset vector index
            _q->rx_vector_index = 0;

            // grab data from port
            _q->rx_vector_length = _q->usrp->get_device()->recv(
                &_q->rx_buffer->front(),
                _q->rx_buffer->size(),
                md,
                uhd::io_type_t::COMPLEX_FLOAT32,
                uhd::device::RECV_MODE_ONE_PACKET
            );
            //printf("reading data from usrp : rx vector length : %u\n", _q->rx_vector_length);

            //handle the error codes
            switch(md.error_code){
            case uhd::rx_metadata_t::ERROR_CODE_NONE:
            case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
                break;
            default:
                std::cerr << "Error code: " << md.error_code << std::endl;
                std::cerr << "Unexpected error on recv, exit test..." << std::endl;
                exit(1);
            }

            if (_q->rx_vector_length == 0) {
                printf("WARNING: vector length is zero!!!\n");
                usleep(10000);
            }

        }

        // for now copy vector "buff" to array of complex float
        // TODO : apply bandwidth-dependent gain
        //num_accumulated_samples++;

        // push 2 samples into buffer
        rx_buffer_decim[n++] = (*_q->rx_buffer)[_q->rx_vector_index++];

        if (n==2) {
            // reset counter
            n=0;

            // decimate
            resamp2_crcf_decim_execute(_q->rx_decim, rx_buffer_decim, &rx_decim_out);

            // apply resampler
            resamp_crcf_execute(_q->rx_resamp, rx_decim_out, rx_buffer_resamp, &nw);

            // push through synchronizer
            ofdmflexframesync_execute(_q->fs, rx_buffer_resamp, nw);

        } // resamp
    } // while ( _q->rx_running ) {

    _q->usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
#if IQPR_VERBOSE
    printf("iqpr_rx_process() complete\n");
#endif

    pthread_exit(0);
} 



// 
// low-level functionality
//

// transmit packet (generic)
//  _q              :   iqpr object
//  _header         :   header data [8 bytes]
//  _payload        :   payload data
//  _payload_len    :   number of bytes in payload
//  _fgprops        :   frame generator properties (internal 'payload_len' ignored)
void iqpr_txpacket(iqpr _q,
                   unsigned char * _header,
                   unsigned char * _payload,
                   unsigned int _payload_len,
                   ofdmflexframegenprops_s * _fgprops)
{
    //unsigned int i;

    // reset interpolator, resampler (flush buffer)
    resamp2_crcf_clear(_q->tx_interp);
    resamp_crcf_reset(_q->tx_resamp);

    // configure frame generator
    if (_fgprops != NULL)
        memmove(&_q->fgprops, _fgprops, sizeof(ofdmflexframegenprops_s));
    ofdmflexframegen_setprops(_q->fg, &_q->fgprops);

    // set up the metadta flags
    uhd::tx_metadata_t md;
    md.start_of_burst = false;  // never SOB when continuous
    md.end_of_burst   = false;  // 
    md.has_time_spec  = false;  // set to false to send immediately

    // compute 'frame' length (actually length of each symbol)
    unsigned int frame_len = _q->M + _q->cp_len;

    // arrays
    std::complex<float> buffer[frame_len];    // output time series
    std::complex<float> buffer_interp[2*frame_len];
    std::complex<float> buffer_resamp[3*frame_len];
    std::vector<std::complex<float> > buff(256);

    //printf("[tx] sending packet %u...\n", _pid);

    // assemble the frame
    ofdmflexframegen_reset(_q->fg);
    ofdmflexframegen_assemble(_q->fg, _header, _payload, _payload_len);

    // generate the frame
    int last_symbol=0;
    unsigned int zero_pad = (512/frame_len) < 1 ? 1 : (512/frame_len);
    float g0 = 0.07f;

    unsigned int j;
    unsigned int tx_buffer_samples=0;
    while (!last_symbol || zero_pad > 0) {
        if (!last_symbol) {
            // generate symbol
            last_symbol = ofdmflexframegen_writesymbol(_q->fg, buffer);
        } else {
            zero_pad--;
            for (j=0; j<frame_len; j++)
                buffer[j] = 0.0f;
        }

        // interpolate by 2
        for (j=0; j<frame_len; j++)
            resamp2_crcf_interp_execute(_q->tx_interp, buffer[j], &buffer_interp[2*j]);
        
        // resample
        unsigned int nw;
        unsigned int n=0;
        for (j=0; j<2*frame_len; j++) {
            resamp_crcf_execute(_q->tx_resamp, buffer_interp[j], &buffer_resamp[n], &nw);
            n += nw;
        }

        // push samples into buffer
        for (j=0; j<n; j++) {
            buff[tx_buffer_samples++] = g0*buffer_resamp[j];

            if (tx_buffer_samples==256) {
                // reset counter
                tx_buffer_samples=0;

                //send the entire contents of the buffer
                _q->usrp->get_device()->send(
                    &buff.front(), buff.size(), md,
                    uhd::io_type_t::COMPLEX_FLOAT32,
                    uhd::device::SEND_MODE_FULL_BUFF
                );
            }
        }
    }

#if 0
    // send a mini EOB packet
    md.start_of_burst = false;
    md.end_of_burst   = true;
    _q->usrp->get_device()->send("", 0, md,
        uhd::io_type_t::COMPLEX_FLOAT32,
        uhd::device::SEND_MODE_FULL_BUFF
    );
#endif
}

// receive data packet with timeout, returning 1 if found, 0 if not
//  _q              :   iqpr object
//  _timespec       :   time specifier
//  _header         :   received header
//  _header_valid   :   header valid?
//  _payload        :   output payload data
//  _payload_len    :   number of bytes in payload
//  _payload_valid  :   payload valid?
//  _stats          :   received frame statistics
int iqpr_rxpacket(iqpr _q,
                  unsigned int     _timespec,
                  unsigned char ** _header,
                  int           *  _header_valid,
                  unsigned char ** _payload,
                  unsigned int  *  _payload_len,
                  int           *  _payload_valid,
                  framesyncstats_s * _stats)
{
    // get current time (timeval)
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    // add offset (timespec)
    struct timespec ts_timeout;
    ts_timeout.tv_sec  = tv_now.tv_sec;         // seconds
    ts_timeout.tv_nsec = (tv_now.tv_usec + _timespec) * 1000; // nanoseconds

    // accumulate nanoseconds into seconds
    while (ts_timeout.tv_nsec > 1000000000) {
        ts_timeout.tv_nsec -= 1000000000;
        ts_timeout.tv_sec++;
    }

    // set flag specifying that we are waiting for a packet, otherwise
    // all incoming packets are ignored
    _q->rx_waiting = 1;

    // lock mutex
    pthread_mutex_lock(&_q->rx_mutex);

    // start timed wait
    int status = pthread_cond_timedwait(&_q->rx_cond, &_q->rx_mutex, &ts_timeout);

    status = _q->rx_packet_found;

    // check status flag
    if (_q->rx_packet_found) {
        // found packet; set outputs
        *_header        = _q->rx_header;
        *_header_valid  = _q->rx_header_valid;
        *_payload       = _q->rx_payload_valid ? _q->rx_payload     : NULL;
        *_payload_len   = _q->rx_payload_valid ? _q->rx_payload_len : 0;
        *_payload_valid = _q->rx_payload_valid;

        // return frame stats
        memmove(_stats, &_q->rx_stats, sizeof(framesyncstats_s));

        // reset status flag
        _q->rx_packet_found = 0;

        memmove(_stats, &_q->rx_stats, sizeof(framesyncstats_s));
    }
    
    // unlock mutex
    pthread_mutex_unlock(&_q->rx_mutex);

    // packet found?
    // NOTE: because internal rx_packet_found is reset, we need to
    //       return this status variable
    return status;

}


// 
// iqpr internal methods
//

// iqpr internal callback method
int iqpr_callback(unsigned char *  _rx_header,
                  int              _rx_header_valid,
                  unsigned char *  _rx_payload,
                  unsigned int     _rx_payload_len,
                  int              _rx_payload_valid,
                  framesyncstats_s _stats,
                  void *           _userdata)
{
    iqpr q = (iqpr) _userdata;

#if IQPR_VERBOSE
    unsigned int pid = (_rx_header[0] << 8) | _rx_header[1];
    printf("********* callback invoked, h[%c], p[%c], pid = %6u%s\n",
            _rx_header_valid  ? ' ' : 'x',
            _rx_payload_valid ? ' ' : 'x',
            pid,
            q->rx_waiting ? " ***" : "");
    //printf("********* callback invoked, ");
    // ...
#endif
    
    if (!q->rx_waiting) {
        // ignore packet
        return 0;
    }

    // set internal values
    q->rx_packet_found  = 1;
    q->rx_header_valid  = _rx_header_valid;
    q->rx_payload_valid = _rx_payload_valid;

    // copy header (regardless of validity)
    memmove(q->rx_header, _rx_header, 8*sizeof(unsigned char));

    // save statistics
    memmove(&q->rx_stats, &_stats, sizeof(framesyncstats_s));

    // re-allocate memory arrays if necessary
    if ( _rx_header_valid &&
         _rx_payload_valid &&
         _rx_payload_len > q->rx_payload_numalloc)
    {
        // header and payload are valid, but we don't have enough
        // space to make the copy. Increase the size of the internal
        // buffer to accommodate the payload.
        q->rx_payload_numalloc = _rx_payload_len;
        unsigned char * tmp = (unsigned char*) realloc(q->rx_payload, q->rx_payload_numalloc*sizeof(unsigned char));

        // check the re-allocation
        if (tmp == NULL) {
            fprintf(stderr,"error: iqpr_callback(), could not allocate payload buffer\n");
            exit(1);
        }
        q->rx_payload = tmp;
    }

    q->rx_payload_len = _rx_payload_len;
    if ( _rx_header_valid && _rx_payload_valid) {
        // copy data (regardless of validity)
        memmove(q->rx_payload, _rx_payload, _rx_payload_len*sizeof(unsigned char));
    }

    // signal condition
    pthread_cond_signal(&q->rx_cond);

    return 0;
}
