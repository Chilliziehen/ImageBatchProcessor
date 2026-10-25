const vec3 LUMA=vec3(.2126,.7152,.0722);
vec3 encode(vec3 c){c=max(c,vec3(0));return mix(12.92*c,1.055*pow(c,vec3(1./2.4))-.055,greaterThan(c,vec3(.0031308)));}
vec3 decode(vec3 c){return mix(c/12.92,pow(max((c+.055)/1.055,vec3(0)),vec3(2.4)),greaterThan(c,vec3(.04045)));}
vec3 rgbhsl(vec3 c){float hi=max(c.r,max(c.g,c.b)),lo=min(c.r,min(c.g,c.b)),d=hi-lo,l=(hi+lo)*.5;float h=0,s=0;if(d>1e-7){s=d/max(1-abs(2*l-1),1e-6);h=hi==c.r?mod((c.g-c.b)/d,6):(hi==c.g?(c.b-c.r)/d+2:(c.r-c.g)/d+4);h/=6;}return vec3(h,s,l);}
vec3 hslrgb(vec3 h){vec3 v=clamp(abs(mod(h.x*6+vec3(0,4,2),6)-3)-1,0,1);return h.z+(v-.5)*(1-abs(2*h.z-1))*h.y;}
