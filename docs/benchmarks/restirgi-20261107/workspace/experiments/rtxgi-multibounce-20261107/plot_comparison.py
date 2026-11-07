"""Scientific figure from actual linear raw captures; one common display transform."""
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
R=Path(__file__).resolve().parent
def read(p):return np.asarray(np.memmap(R/p,dtype='<f4',mode='r',shape=(1080,1920,4))[:,:,:3])
images=[('PT reference | 8192 spp',read('analysis/common-reference-8192spp.rgba32f')),
 ('RestirGI | full-res benchmark',read('runs/reference-quality-r1-v1/raw/frame_000127/raw_gi.bin')),
 ('RTXPT ReSTIR GI',read('runs/rtxpt-quality-r1-v1/gi-frame-127.rgba32f')),
 ('RTXDI ReSTIR GI',read('runs/rtxdi-quality-r1-v1/capture.frame127.rgba32f')),
 ('Corona | current port',read('runs/corona-port-quality-r1-seedstreams-v1/raw/frame_0127/raw_indirect.f32'))]
def display(a):
    a=np.maximum(a*8,0);a=a/(1+a)
    return np.where(a<=.0031308,12.92*a,1.055*a**(1/2.4)-.055)
x0,y0,x1,y1=700,590,1300,1000
fig,axes=plt.subplots(2,5,figsize=(25,8),gridspec_kw={'height_ratios':[.5625,.68333]},layout='constrained')
fig.set_constrained_layout_pads(w_pad=.08,h_pad=.08,hspace=.02,wspace=.01)
for i,(label,a) in enumerate(images):
    axes[0,i].imshow(display(a));axes[0,i].set_title(label,fontsize=14)
    axes[0,i].add_patch(Rectangle((x0,y0),x1-x0,y1-y0,edgecolor='#e8bd36',facecolor='none',linewidth=1))
    axes[1,i].imshow(display(a[y0:y1,x0:x1]));axes[1,i].set_title('Same crop',fontsize=11)
    for ax in axes[:,i]:ax.axis('off')
fig.suptitle('Kitchen | 1920 x 1080 | min_depth 5 / max_depth 16 / RR 1 | indirect only | denoising off\n1 new candidate path per pixel/frame; native reuse controls retained | Repeat 1, Frame 127 | Shared exposure 8 + shared Reinhard/sRGB display',fontsize=14)
fig.savefig(R/'comparison-multibounce.png',dpi=150,facecolor='white');plt.close(fig)
fig,axes=plt.subplots(1,4,figsize=(20,4))
ref=images[0][1]
for ax,(label,a) in zip(axes,images[1:]):
    error=(a-ref)@np.array([.2126,.7152,.0722],dtype=np.float32)
    im=ax.imshow(error,cmap='RdBu_r',vmin=-.02,vmax=.02);ax.set_title(label);ax.axis('off')
fig.colorbar(im,ax=axes,shrink=.75,label='Signed linear luminance error (same +/-0.02 scale)')
fig.suptitle('Single-frame error to common PT reference: noise and bias together')
fig.savefig(R/'error-multibounce.png',dpi=150,facecolor='white',bbox_inches='tight');plt.close(fig)
print(R/'comparison-multibounce.png')
