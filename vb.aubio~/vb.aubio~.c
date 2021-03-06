#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "ext_common.h"
#include "buffer.h"
#include "ext_buffer.h"
#include <aubio/aubio.h>

#include <Accelerate/Accelerate.h>

/*
	using the aubio framework (Paul Brossier) for analyzing audio buffers
	by volker böhm
 
*/


#define MAXSIZE 4096

typedef struct {
	t_object		b_obj;
	
	uint_t			overlap;
	smpl_t			silence;			// onset detection silence threshold
	smpl_t			thresh;			// detection threshold
	smpl_t			minioi;			// mininum inter onset interval in ms
	t_symbol		*method;		// onset detection method
	
	t_buffer_ref		*bufref;			// buffer ref
	t_symbol		*bufname;		// buffer name
	void				*outA, *outB, *outC;
	t_atom			*onsetList;

} t_myObj;


t_symbol *ps_buffer_modified;

static t_class *myObj_class;

void myObj_bang(t_myObj *x);
void myObj_pitch(t_myObj *x);
void myObj_setOverlap(t_myObj *x, long input);
void do_onset(t_myObj *x);
void myObj_setBuf(t_myObj *x, t_symbol *s);
void myObj_dblclick(t_myObj *x);
t_max_err myObj_notify(t_myObj *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void myObj_free(t_myObj *x);
void myObj_assist(t_myObj *x, void *b, long m, long a, char *s);
void *myObj_new( t_symbol *s, long argc, t_atom *argv);


int C74_EXPORT main(void) {
	t_class *c;
	
	c = class_new("vb.aubio~", (method)myObj_new, (method)myObj_free, (short)sizeof(t_myObj), 
				  0L, A_GIMME, 0L);
	class_addmethod(c, (method)myObj_bang, "bang", 0);
	//class_addmethod(c, (method)myObj_pitch, "pitch", 0);
	class_addmethod(c, (method)myObj_setOverlap, "overlap", A_LONG, 0);
	class_addmethod(c, (method)myObj_setBuf, "set", A_SYM, 0);
	class_addmethod(c, (method)myObj_dblclick, "dblclick", A_CANT, 0);
	class_addmethod(c, (method)myObj_assist, "assist", A_CANT,0);
	class_addmethod(c, (method)myObj_notify, "notify", A_CANT, 0);
	
	
	// attributes ==== 
	CLASS_ATTR_FLOAT(c,"mininterval", 0, t_myObj, minioi);
	CLASS_ATTR_SAVE(c, "mininterval", 0);
	CLASS_ATTR_FILTER_MIN(c,"mininterval", 8); 
	CLASS_ATTR_LABEL(c,"mininterval",0, "minimum interval (ms)");
	
	CLASS_ATTR_FLOAT(c,"silence", 0, t_myObj, silence);
	CLASS_ATTR_SAVE(c, "silence", 0);
	CLASS_ATTR_FILTER_CLIP(c,"silence",-90,-20); 
	CLASS_ATTR_LABEL(c,"silence",0, "silence threshold (dB)");
	
	CLASS_ATTR_SYM(c,"mode", 0, t_myObj, method);
	CLASS_ATTR_ENUM(c, "mode", 0, "complex energy hfc specdiff phase kl mkl");
	CLASS_ATTR_SAVE(c, "mode", 0);
	CLASS_ATTR_LABEL(c,"mode",0, "detection method");
	
	
	CLASS_ATTR_FLOAT(c,"thresh", 0, t_myObj, thresh);
	CLASS_ATTR_SAVE(c, "thresh", 0);
	CLASS_ATTR_FILTER_CLIP(c,"thresh",0,10); 
	CLASS_ATTR_LABEL(c,"thresh",0, "detection threshold");
	
	// display order
	CLASS_ATTR_ORDER(c, "thresh",	0, "1");
	CLASS_ATTR_ORDER(c, "mode",	0, "2");
	CLASS_ATTR_ORDER(c, "silence", 0, "3");
	CLASS_ATTR_ORDER(c, "mininterval", 0, "4");
	
	
	class_register(CLASS_BOX, c);
	myObj_class = c;
	
	ps_buffer_modified = gensym("buffer_modified");
	
	post("vb.aubio~ using aubio 0.4.2 by Paul Brossier");
	
	return 0;
}


void myObj_setOverlap(t_myObj *x, long input) 
{
	switch (input) {
		case 2:
			x->overlap = 2;
			break;
		case 4:
			x->overlap = 4;
			break;
		case 8:
			x->overlap = 8;
			break;
		default:
			x->overlap = 4;
			post("overlap must be either 2, 4, or 8 -- resetting to 4");
			break;
	}
}


void myObj_bang(t_myObj *x) {
	defer_low(x, (method)do_onset, NULL,0, NULL);
}

#pragma mark onset detection function -------------
void do_onset(t_myObj *x) 
{
	uint_t		i, n;				// frame counter
	t_float		*tab;
	long		frames, nchnls;
	t_buffer_obj	*b;			
	uint_t		win_s = 512;
	uint_t		hop_s = win_s / x->overlap;	// hop size
	uint_t		numframes, sr, numonsets;
	smpl_t		onsettime;		// onset time in ms
	smpl_t		is_onset;
	fvec_t		*input, *onset;
	aubio_onset_t *o;
	float		rms, max;

	
	b = buffer_ref_getobject(x->bufref);
	if(!b) {
		object_error((t_object *)x,"%s is no valid buffer", x->bufname->s_name);	
		return ;
	}
	tab = buffer_locksamples(b);				// access samples in buffer
	if (!tab) {
		object_error((t_object *)x, "can't access buffer %s", x->bufname->s_name);
		return;
	}
	
	frames = buffer_getframecount(b);		// buffer size in samples
	nchnls = buffer_getchannelcount(b);		// number of channels
	sr = buffer_getsamplerate(b);			// sampling rate
	
	
	// calc overall RMS
	t_buffer *bufstruct = (t_buffer *)b;
	t_symbol *original_fname = bufstruct->b_filename;
	vDSP_rmsqv(tab, nchnls, &rms, frames);
	vDSP_maxmgv(tab, nchnls, &max, frames);
	object_post((t_object*)x, "%s: rms: %f -- max: %f", original_fname->s_name, rms, max);
	
	//
	
	
	// create some vectors
	input = new_fvec (hop_s);				// input buffer
	onset = new_fvec (1);					// output candidates
	
	// create onset object
	o = new_aubio_onset (x->method->s_name, win_s, hop_s, sr);
	
	aubio_onset_set_threshold (o, x->thresh);
	aubio_onset_set_silence (o, x->silence);
	aubio_onset_set_minioi_ms(o, x->minioi);
	

	numframes = (frames - win_s) / hop_s;
	numonsets = n = 0;

	// 2. do something with it
	while (n < numframes) 
	 {
		// get `hop_s` new samples into `input`
		int offset = n*hop_s;
		for(i=0; i<hop_s; i++) {
			input->data[i] = tab[(i+offset)*nchnls];	// only read from channel 1... TODO: think about it!------
		}

		// execute onset detection
		aubio_onset_do (o, input, onset);
		
		is_onset = fvec_get_sample(onset, 0);
		if( is_onset ) {
			//time = offset + is_onset*hop_s - aubio_onset_get_delay(o);
			//onsettime = aubio_onset_get_last(o); // onset time in samples
			
			onsettime = aubio_onset_get_last_ms(o);	// get onset time in ms

			atom_setfloat(x->onsetList+numonsets, onsettime);
			numonsets++;
			if(numonsets>=MAXSIZE) {
				object_warn((t_object*)x, "number of onsets maxed out!");
				break;
			}
		}
		
		n++;
	}
	
	buffer_unlocksamples(b);
	
	if(numonsets>0)
		outlet_list(x->outA, 0L, numonsets, x->onsetList);
	else
		object_post((t_object*)x, "no onsets found!");
	
	
	// 3. clean up memory
	del_aubio_onset(o);
	del_fvec(onset);
	del_fvec(input);
	aubio_cleanup();
	
	outlet_bang(x->outB);			// bang when done
}



#pragma mark pitch detection function -------------
// pitch not ready yet------------
void myObj_pitch(t_myObj *x) {
	uint_t		i;
	t_float		*tab;
	long		frames, nchnls;
	t_buffer_obj	*b;
	
	uint_t		n = 0;				// frame counter
	uint_t		win_s = 2048;		// window size
	uint_t		hop_s = win_s / x->overlap;	// hop size
	uint_t		numframes, sr;
	fvec_t		*input, *output;
	aubio_pitch_t *o;
	
	
	b = buffer_ref_getobject(x->bufref);
	if(!b) {
		object_error((t_object *)x,"%s is no valid buffer", x->bufname->s_name);	
		return ;
	}
	tab = buffer_locksamples(b);				// access samples in buffer
	if (!tab) {
		return;
	}
	
	frames = buffer_getframecount(b);		// buffer size in samples
	nchnls = buffer_getchannelcount(b);		// number of channels
	sr = buffer_getsamplerate(b);
	
	
	// create some vectors
	input = new_fvec (hop_s); // input buffer
	output = new_fvec (1); // output candidates
	// create pitch object
	o = new_aubio_pitch ("default", win_s, hop_s, sr);
	
	numframes = (frames - win_s) / hop_s;
	
	
	// 2. do something with it
	while (n < numframes) {
		// get `hop_s` new samples into `input`
		int offset = n*hop_s;
		for(i=0; i<hop_s; i++) {
			input->data[i] = tab[(i+offset)*nchnls];
		}
		// exectute pitch
		aubio_pitch_do (o, input, output);
		//	TODO: think about way to output pitch info
		//outlet_float(x->outC, output->data[0]);
		//outlet_int(x->outB, offset);
		
		n++;
	};
	
	
	buffer_unlocksamples(b);
	
	// 3. clean up memory
	del_aubio_pitch(o);
	del_fvec(output);
	del_fvec(input);
	aubio_cleanup();
}


void myObj_setBuf(t_myObj *x, t_symbol *s)
{
	if (!x->bufref)
		x->bufref = buffer_ref_new((t_object*)x, s);
	else
		buffer_ref_set(x->bufref, s);
	
	x->bufname = s;		
}


void myObj_dblclick(t_myObj *x)
{
	buffer_view(buffer_ref_getobject(x->bufref));
}


t_max_err myObj_notify(t_myObj *x, t_symbol *s, t_symbol *msg, void *sender, void *data)
{
	/*
	post("notify message received!");
	if (msg == ps_buffer_modified) {
		post("buffer modified received!");
		
		t_symbol *bufname = (t_symbol *)object_method((t_object*)sender, gensym("getname"));
		post("bufname: %s", bufname->s_name);
		
		
		 t_buffer_obj *b = (t_buffer_obj *)data;
		 post("channels: %d", buffer_getchannelcount(b));
		t_buffer_info *info = NULL;
		 t_max_err err = buffer_getinfo(b, info);
		 post("err: %ld", err);
		//post("nochmal channels: %d", info->b_nchans);
		if(info) {
			t_symbol *bname = info->b_name;
			post("buffer name: %s", bname->s_name);
		}
		
	}*/
	return buffer_ref_notify(x->bufref, s, msg, sender, data);
}


void myObj_free(t_myObj *x) {
	if(x->onsetList)
		sysmem_freeptr(x->onsetList);
	if(x->bufref) object_free(x->bufref);
}


void myObj_assist(t_myObj *x, void *b, long m, long a, char *s) {
	if (m == ASSIST_INLET) {
		switch(a) {
			case 0: sprintf (s,"bang to start processing"); break;
		}
	}
	else {
		switch(a) {
			case 0: sprintf (s,"bang when done processing"); break;
			case 1: sprintf(s, "(list) onset times in ms"); break;
				//case 2: sprintf(s, "(float) pitch in Hz at sample offset"); break;
		}
		
	}
}


void *myObj_new( t_symbol *s, long argc, t_atom *argv)
{
	int i;
	t_myObj *x = object_alloc(myObj_class);
	if(x) {		
		//x->outC = floatout(x);
		//x->outB = intout(x);
		x->outA = listout(x);
		x->outB = bangout(x);
		

		x->silence = -70.;
		x->minioi = 14.3;
		x->method = gensym("hfc");
		x->thresh = 1.;
		x->overlap = 4;
		x->bufref = NULL;
		
		x->onsetList = (t_atom *) sysmem_newptr(MAXSIZE*sizeof(t_atom));
		for (i=0; i < MAXSIZE; i++)
			atom_setlong(x->onsetList+i, 0);
		
		
		if(argc>=1) {
			if(atom_gettype(argv) == A_SYM)
				myObj_setBuf(x, atom_getsym(argv));
			else 
				object_error((t_object *)x, "missing buffer name argument!");
		}

		
		attr_args_process(x, argc, argv+1);			// process attributes
	}
	
	else {
		object_free(x);
		x = NULL;
	}
	
	
	return x;
}


