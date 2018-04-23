/*
 * Copyright 2014-2016 Pascal Gauthier
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * *distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/


#include <fstream>
#include "m_pd.h"
#include "../WDL/mutex.h"
#include "../jsusfx.h"
#include <stdarg.h>

class JsusFxPd : public JsusFx {
public:
    void displayMsg(const char *fmt, ...) {
        char output[4096];
        va_list argptr;
        va_start(argptr, fmt);
        vsnprintf(output, 4095, fmt, argptr);
        va_end(argptr);

        post(output);
    }

    void displayError(const char *fmt, ...) {
        char output[4096];
        va_list argptr;
        va_start(argptr, fmt);
        vsnprintf(output, 4095, fmt, argptr);
        va_end(argptr);

        error("%s", output);
    }

    WDL_Mutex dspLock;
};

typedef struct _jsusfx {
    t_object x_obj;
    t_float x_f;
    JsusFxPd *fx;
    char canvasdir[2048];
    char scriptpath[2048];
    bool bypass;
    bool user_bypass;
} t_jsusfx;

static t_class *jsusfx_class;
static t_class *jxrt_class;
static t_class *inlet_proxy;

typedef struct _inlet_proxy {
    t_object x_obj;
    t_jsusfx *peer;
    int idx;
} t_inlet_proxy;

void jsusfx_describe(t_jsusfx *x) {
    post("jsusfx~ script %s : %s", x->scriptpath, x->fx->desc);
    for(int i=0;i<64;i++) {
        if ( x->fx->sliders[i].exists ) {
            JsusFx_Slider *s = &(x->fx->sliders[i]);
            if ( s->inc == 0 )
                post(" slider%d: %g %g %s [%g]", i, s->min, s->max, s->desc, *(s->owner));
            else
                post(" slider%d: %g %g (%g) %s [%g]", i, s->min, s->max, s->inc, s->desc, *(s->owner));
        }
    }
}

void jsusfx_dumpvars(t_jsusfx *x) {
    post("jsusfx~ vars for: %s =========", x->fx->desc);
    x->fx->dumpvars();
}

void jsusfx_compile(t_jsusfx *x, t_symbol *newFile) {
    x->bypass = true;

    std::ifstream *is;

    if ( newFile != NULL && newFile->s_name[0] != 0) {
        char result[1024], *bufptr;
        result[0] = 0;
        int fd = open_via_path(x->canvasdir, newFile->s_name, "", result, &bufptr, 1024, 1);
        if ( fd < 0 || result[0] == 0 ) {
            error("jsusfx~: unable to find script %s", newFile->s_name);
            return;
        }
        sys_close(fd);
        strncat(result, "/", 1024);
        strncat(result, newFile->s_name, 1024);

        is = new std::ifstream(result);
        if ( ! is->is_open() ) {
            error("jsusfx~: error opening file %s", result);
            delete is;
            return;
        }
        strncpy(x->scriptpath, result, 1024);
    } else {
        if ( x->scriptpath[0] == 0 )
            return;
        is = new std::ifstream(x->scriptpath);
        if ( ! is->is_open() ) {
            error("jsusfx~: error opening file %s", x->scriptpath);
            delete is;
            return;
        }
    }

    x->fx->dspLock.Enter();
    if ( x->fx->compile(*is) ) {
        if ( x->fx->srate != 0 )
            x->fx->prepare(*(x->fx->srate), *(x->fx->samplesblock));
        x->bypass = false;
    } else {
        x->bypass = true;
    }
    x->fx->dspLock.Leave();

    delete is;

    if ( ! x->bypass )
        jsusfx_describe(x);
}

void jsusfx_slider(t_jsusfx *x, t_float id, t_float value) {
    int i = (int) id;

    if ( i > 64 || i < 0 )
        return;

    if ( ! x->fx->sliders[i].exists ) {
        error("jsusfx~: slider number %d is not assigned for this effect", i);
        return;
    }
    x->fx->moveSlider(i, value);
}

void jsusfx_bypass(t_jsusfx *x, t_float id) {
    x->user_bypass = id != 0;
}

t_int *jsusfx_perform(t_int *w) {
    float *ins[2];
    float *outs[2];

    t_jsusfx *x = (t_jsusfx *)(w[1]);
    ins[0] = (float *)(w[2]);
    ins[1] = (float *)(w[3]);
    outs[0] = (float *)(w[4]);
    outs[1] = (float *)(w[5]);
    int n = (int)(w[6]);

    if ( (x->bypass || x->user_bypass) || x->fx->dspLock.TryEnter() ) {
        //x->fx->displayMsg("system is bypassed");

        for(int i=0;i<n;i++) {
            outs[0][i] = ins[0][i];
            outs[1][i] = ins[1][i];
        }
    } else {
        x->fx->process(ins, outs, n, 2, 2);
        x->fx->dspLock.Leave();
    }

    return (w+7);
}

void jsusfx_dsp(t_jsusfx *x, t_signal **sp) {
    x->fx->prepare(sp[0]->s_sr, sp[0]->s_n);
    dsp_add(jsusfx_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec, sp[0]->s_n);
}

void *jsusfx_new(t_symbol *notused, long argc, t_atom *argv) {
    JsusFxPd *fx = new JsusFxPd();

    fx->normalizeSliders = 1;
    t_jsusfx *x = (t_jsusfx *)pd_new(jsusfx_class);

    t_symbol *dir = canvas_getcurrentdir();
    strcpy(x->canvasdir, dir->s_name);
    x->fx = fx;
    x->bypass = true;
    x->user_bypass = false;
    x->scriptpath[0] = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, gensym("signal"));
    outlet_new(&x->x_obj, gensym("signal"));

    if ( argc < 1 || (argv[0]).a_type != A_SYMBOL ) {
        error("jsusfx~: missing script");
        return x;
    }

    t_symbol *s = atom_getsymbol(argv);
    jsusfx_compile(x, s);

    return (x);
}

void jsusfx_free(t_jsusfx *x) {
    delete x->fx;
}

void *jxrt_new(t_symbol *script) {
    JsusFxPd *fx = new JsusFxPd();

    fx->normalizeSliders = 0;
    t_jsusfx *x = (t_jsusfx *)pd_new(jxrt_class);
    
    t_symbol *dir = canvas_getcurrentdir();
    strcpy(x->canvasdir, dir->s_name);
    x->fx = fx;
    x->bypass = true;
    x->user_bypass = false;

    jsusfx_compile(x, script);
    if ( x->bypass == true ) {
        delete fx;
        return NULL;
    }
    
    // TODO: support multiple signal inlets/outlets
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, gensym("signal"));
    outlet_new(&x->x_obj, gensym("signal"));

    for(int i=1;i<64;i++) {
        if ( x->fx->sliders[i].exists ) {
            t_inlet_proxy *proxy = (t_inlet_proxy *) pd_new(inlet_proxy);
            proxy->idx = i;
            proxy->peer = x;
            inlet_new(&x->x_obj, &proxy->x_obj.ob_pd, 0, 0);
        } else {
            break;
        }
    }

    return (x);
}

void jxrt_free(t_jsusfx *x) {
    // TODO: free signal inlets/outlets ; also the proxy inlets ?
    delete x->fx;
}

static void inlet_float(t_inlet_proxy *proxy, t_float f) {
    proxy->peer->fx->moveSlider(proxy->idx, f);
}

extern "C" {
    void jsusfx_tilde_setup(void) {
        jsusfx_class = class_new(gensym("jsusfx~"), (t_newmethod)jsusfx_new, (t_method)jsusfx_free, sizeof(t_jsusfx), 0L, A_GIMME, 0);
        class_addmethod(jsusfx_class, (t_method)jsusfx_dsp, gensym("dsp"), A_CANT, 0);
        class_addmethod(jsusfx_class, (t_method)jsusfx_slider, gensym("slider"), A_FLOAT, A_FLOAT, 0);
        class_addmethod(jsusfx_class, (t_method)jsusfx_compile, gensym("compile"), A_DEFSYMBOL, 0);
        class_addmethod(jsusfx_class, (t_method)jsusfx_describe, gensym("describe"), A_NULL, 0);
        class_addmethod(jsusfx_class, (t_method)jsusfx_dumpvars, gensym("dumpvars"), A_NULL, 0);
        class_addmethod(jsusfx_class, (t_method)jsusfx_bypass, gensym("bypass"), A_FLOAT, 0);
        CLASS_MAINSIGNALIN(jsusfx_class, t_jsusfx, x_f);

        jxrt_class = class_new(gensym("jxrt~"), (t_newmethod)jxrt_new, (t_method)jxrt_free, sizeof(t_jsusfx), 0L, A_SYMBOL, 0);
        class_addmethod(jxrt_class, (t_method)jsusfx_dsp, gensym("dsp"), A_CANT, 0);
        class_addmethod(jxrt_class, (t_method)jsusfx_bypass, gensym("bypass"), A_FLOAT, 0);
        class_addmethod(jxrt_class, (t_method)jsusfx_describe, gensym("describe"), A_NULL, 0);
        class_addmethod(jxrt_class, (t_method)jsusfx_dumpvars, gensym("dumpvars"), A_NULL, 0);
        CLASS_MAINSIGNALIN(jxrt_class, t_jsusfx, x_f);

        inlet_proxy = class_new(gensym("jxrt_inlet_proxy"), NULL,NULL, sizeof(t_inlet_proxy), CLASS_PD|CLASS_NOINLET, A_NULL);
        class_addfloat(inlet_proxy, (t_method)inlet_float);

        JsusFx::init();
    }
}
