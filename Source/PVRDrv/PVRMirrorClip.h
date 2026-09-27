// Final TA-stream clipping for reflection views. Non-reflection draws bypass it.
// PVRMirrorWrite is supplied by the renderer (or the host regression harness).
static FSceneNode* GPVRMirrorFrame = NULL;
static FLOAT GPVRMirrorDepthScale = 1.f;
static INT GPVRMirrorStripCount = 0;
static pvr_vertex_t GPVRMirrorPrevious[2];
static pvr_vertex_t GPVRMirrorScratch __attribute__((aligned(32)));

static DWORD PVRMirrorColor(DWORD A, DWORD B, FLOAT T)
{
	DWORD Result=0;
	for( INT Shift=0; Shift<32; Shift+=8 )
	{
		const INT X=(A>>Shift)&255, Y=(B>>Shift)&255;
		Result |= (DWORD)Clamp<INT>((INT)(X+(Y-X)*T+0.5f),0,255)<<Shift;
	}
	return Result;
}

static pvr_vertex_t PVRMirrorLerp(const pvr_vertex_t& A,const pvr_vertex_t& B,FLOAT T)
{
	pvr_vertex_t R=A;
	R.x=A.x+(B.x-A.x)*T; R.y=A.y+(B.y-A.y)*T;
	R.z=A.z+(B.z-A.z)*T;
	// TA texture coordinates are perspective-correct; clipping in screen space
	// must interpolate U/W and V/W, not raw U,V. Colors are screen-linear.
	R.u=(A.u*A.z+(B.u*B.z-A.u*A.z)*T)/R.z;
	R.v=(A.v*A.z+(B.v*B.z-A.v*A.z)*T)/R.z;
	R.argb=PVRMirrorColor(A.argb,B.argb,T);
	R.oargb=PVRMirrorColor(A.oargb,B.oargb,T);
	return R;
}

static void PVRMirrorTriangle(FSceneNode* Frame,const pvr_vertex_t* Triangle)
{
	while( Frame && !Frame->DCMirrorApertures ) Frame=Frame->Parent;
	if( !Frame )
	{
		for( INT i=0; i<3; ++i )
		{
			pvr_vertex_t V=Triangle[i];
			V.z*=GPVRMirrorDepthScale;
			V.flags=i==2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
			PVRMirrorWrite(&V);
		}
		return;
	}
	// BSP pieces partition the opening. Intersect with every ancestor opening
	// as well, so nested reflections cannot leak outside their parent mirror.
	for( const FDCPortalAperture* A=Frame->DCMirrorApertures; A; A=A->Next )
	{
		check(A->Count<=FBspNode::MAX_FINAL_VERTICES);
		if( A->Count<3 ) continue;
		FLOAT Area=0;
		for( INT e=0; e<A->Count; ++e )
		{ const FVector& P=A->Points[e]; const FVector& Q=A->Points[(e+1)%A->Count]; Area+=P.X*Q.Y-Q.X*P.Y; }
		if( Area==0 ) continue;
		const FLOAT Sign=Area>0 ? 1.f : -1.f;
		// A triangle clipped against N edges has at most N+3 vertices.
		pvr_vertex_t Buffers[2][FBspNode::MAX_FINAL_VERTICES+4];
		INT Count=3, Bank=0;
		for( INT i=0; i<3; ++i ) Buffers[0][i]=Triangle[i];
		for( INT e=0; e<A->Count && Count>=3; ++e )
		{
			const FVector& P=A->Points[e]; const FVector& Q=A->Points[(e+1)%A->Count];
			pvr_vertex_t* In=Buffers[Bank]; pvr_vertex_t* Out=Buffers[Bank^1];
			INT N=0;
			pvr_vertex_t Previous=In[Count-1];
			FLOAT D0=Sign*((Q.X-P.X)*(Previous.y-P.Y)-(Q.Y-P.Y)*(Previous.x-P.X));
			for( INT i=0; i<Count; ++i )
			{
				const pvr_vertex_t& V=In[i];
				const FLOAT D1=Sign*((Q.X-P.X)*(V.y-P.Y)-(Q.Y-P.Y)*(V.x-P.X));
				if( (D0<0)!=(D1<0) ) Out[N++]=PVRMirrorLerp(Previous,V,D0/(D0-D1));
				if( D1>=0 ) Out[N++]=V;
				Previous=V; D0=D1;
			}
			Count=N; Bank^=1;
		}
		for( INT i=2; i<Count; ++i )
		{
			pvr_vertex_t T[3]={Buffers[Bank][0],Buffers[Bank][i-1],Buffers[Bank][i]};
			PVRMirrorTriangle(Frame->Parent,T);
		}
	}
}

static void PVRMirrorVertex(const pvr_vertex_t& V)
{
	if( GPVRMirrorStripCount<2 ) GPVRMirrorPrevious[GPVRMirrorStripCount]=V;
	else
	{
		pvr_vertex_t T[3]={GPVRMirrorPrevious[0],GPVRMirrorPrevious[1],V};
		if( GPVRMirrorStripCount&1 ) { T[0]=GPVRMirrorPrevious[1]; T[1]=GPVRMirrorPrevious[0]; }
		if( (T[1].x-T[0].x)*(T[2].y-T[0].y)!=(T[1].y-T[0].y)*(T[2].x-T[0].x) )
			PVRMirrorTriangle(GPVRMirrorFrame,T);
		GPVRMirrorPrevious[0]=GPVRMirrorPrevious[1]; GPVRMirrorPrevious[1]=V;
	}
	++GPVRMirrorStripCount;
	if( V.flags==PVR_CMD_VERTEX_EOL ) GPVRMirrorStripCount=0;
}

static void PVRMirrorSetFrame(FSceneNode* Frame)
{
	check(GPVRMirrorStripCount==0);
	GPVRMirrorFrame=NULL; GPVRMirrorDepthScale=1.f;
	for( FSceneNode* F=Frame; F; F=F->Parent )
		if( F->DCMirrorApertures )
		{
			GPVRMirrorFrame=Frame;
			if( F->DCMirrorView ) GPVRMirrorDepthScale*=1.f/16777216.f;
		}
}
