
cbuffer UV:register(b0){uint4 uv[3];};
cbuffer Geometry:register(b1){uint4 geometry[3];};
cbuffer Frame:register(b2){uint4 frame[12];};
cbuffer Material:register(b3){uint4 material[3];};
cbuffer Lighting:register(b4){uint4 lighting[30];};
cbuffer Fog:register(b6){uint4 fogData[4];};
RWByteAddressBuffer packed:register(u0);
#ifdef DBL_CS_CAPTURE
cbuffer Strict:register(b5){uint4 strictData[76];};
RWByteAddressBuffer strictPacked:register(u1);
#endif
[numthreads(32,1,1)]void main(uint3 id:SV_DispatchThreadID){
 uint i=id.x;
 if(i<4)packed.Store4(1536+i*16,fogData[i]);
 if(i<3)packed.Store4(i*16,uv[i]);
 if(i<3)packed.Store4(256+i*16,geometry[i]);
 if(i<12)packed.Store4(512+i*16,frame[i]);
 if(i<3)packed.Store4(768+i*16,material[i]);
 if(i<30)packed.Store4(1024+i*16,lighting[i]);
#ifdef DBL_CS_CAPTURE
 for(uint j=i;j<76;j+=32)strictPacked.Store4(j*16,strictData[j]);
#endif
}
