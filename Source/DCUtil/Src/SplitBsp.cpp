#include <stdlib.h>

#include "DCUtilPrivate.h"

// Experimental compiled-BSP partition. Preserve the collision decision tree
// while dividing drawable polygons and zone-owned actors between travel maps.
// Script state across a travel boundary still requires a map-specific audit.
static INT SplitIndex( const TArray<INT>& Remap, INT Old, const char* What )
{
	if( Old == INDEX_NONE )
		return INDEX_NONE;
	if( Old < 0 || Old >= Remap.Num() || Remap(Old) == INDEX_NONE )
		appErrorf( "TESTSPLIT invalid %s reference %d", What, Old );
	return Remap(Old);
}

// BSP light lists are null-terminated slices of one pointer array. Their roots
// can overlap, so compact each distinct root into a new array and remap every
// surface/leaf root. Merely clearing a removed actor's slot would truncate a
// list and discard any retained lights following it.
static INT SplitLightRoot( const TArray<AActor*>& OldLights, INT Root,
	TArray<INT>& RootRemap, TArray<AActor*>& PackedLights, INT& Removed )
{
	if( Root == INDEX_NONE )
		return INDEX_NONE;
	if( Root < 0 || Root >= OldLights.Num() )
		appErrorf( "TESTSPLIT invalid light-list root %d/%d", Root, OldLights.Num() );
	if( RootRemap(Root) != INDEX_NONE )
		return RootRemap(Root);
	const INT NewRoot = PackedLights.Num();
	RootRemap(Root) = NewRoot;
	for( INT i=Root; i<OldLights.Num(); ++i )
	{
		AActor* Light = OldLights(i);
		if( !Light )
		{
			PackedLights.AddItem(NULL);
			return NewRoot;
		}
		if( Light->bDeleteMe )
			++Removed;
		else
			PackedLights.AddItem(Light);
	}
	appErrorf( "TESTSPLIT unterminated light list at %d", Root );
	return INDEX_NONE;
}

static INT SplitPoint( UModel* Model, INT Old, TArray<INT>& Remap, TArray<FVector>& Packed )
{
	if( Old == INDEX_NONE )
		return INDEX_NONE;
	if( Old < 0 || Old >= Remap.Num() )
		appErrorf( "TESTSPLIT invalid point %d", Old );
	if( Remap(Old) == INDEX_NONE )
		Remap(Old) = Packed.AddItem( Model->Points->Element(Old) );
	return Remap(Old);
}

static INT SplitVector( UModel* Model, INT Old, TArray<INT>& Remap, TArray<FVector>& Packed )
{
	if( Old == INDEX_NONE )
		return INDEX_NONE;
	if( Old < 0 || Old >= Remap.Num() )
		appErrorf( "TESTSPLIT invalid vector %d", Old );
	if( Remap(Old) == INDEX_NONE )
		Remap(Old) = Packed.AddItem( Model->Vectors->Element(Old) );
	return Remap(Old);
}

// Retain every decision plane traversed by a swept line. A removed BSP child
// inherits its parent's CSG outside state, which is not generally equivalent
// to the child subtree's result, even when that subtree has no kept polygons.
static void KeepSplitCollisionLine( UModel* Model, INT iNode, const FVector& Start,
	const FVector& End, TArray<BYTE>& Keep )
{
	while( iNode != INDEX_NONE )
	{
		Keep(iNode) = 1;
		const FBspNode& Node = Model->Nodes->Element(iNode);
		const FLOAT A = Node.Plane.PlaneDot(Start);
		const FLOAT B = Node.Plane.PlaneDot(End);
		if( A >= 0.f && B >= 0.f )
			iNode = Node.iFront;
		else if( A < 0.f && B < 0.f )
			iNode = Node.iBack;
		else
		{
			const FLOAT T = A / (A-B);
			const FVector Middle = Start + (End-Start) * T;
			KeepSplitCollisionLine(Model, Node.iChild[A >= 0.f], Start, Middle, Keep);
			iNode = Node.iChild[A < 0.f];
		}
	}
}

// Include a conservative player cylinder plus separation margin. Checking only
// a marker against its paired exit missed overlapping neighbouring portals.
static UBOOL SplitArrivalClear( ULevel* Level, const FVector& P )
{
	for( INT i=0; i<Level->Num(); ++i )
	{
		const ATeleporter* T=Cast<ATeleporter>(Level->Actors(i));
		if( !T || !T->bEnabled || !T->URL[0] ) continue;
		const FVector D=P-T->Location;
		if( Abs(D.Z)<T->CollisionHeight+56.f
			&& D.X*D.X+D.Y*D.Y<Square(T->CollisionRadius+40.f) ) return 0;
	}
	return 1;
}

// Original SkyTown walk links cross these five openings. Other matching BSP
// polygons include roof/floor boundaries, a window and fragments of the same
// doorway, not twenty-seven separate travel doors. Stable original node IDs.
static UBOOL SkyTravelOpening( INT Node )
{
	return Node==1722 || Node==3749 || Node==7461 || Node==9622 || Node==10271;
}

static void RepairSplitTravel( ULevel* Level )
{
	const char* Map=Level->GetParent()->GetName();
	// Explicit Dig policy from the hardware progression report: no synthetic
	// return over this cut. Static nav links alone cannot model scripted barriers.
	if( !appStricmp(Map,"Dig2") )
		for( INT i=0; i<Level->Num(); ++i )
		{
			AActor* A=Level->Actors(i);
			if( A && !appStrnicmp(A->GetName(),"DCSplitDig2Exit",15) )
			{
				printf("SPLITTRAVEL removed map=%s actor=%s reason=one-way-cut\n",Map,A->GetName());
				A->bDeleteMe=1; Level->Actors(i)=NULL;
			}
		}
	if( !appStricmp(Map,"SkyTown1") || !appStricmp(Map,"SkyTown2") )
		for( INT i=0; i<Level->Num(); ++i )
		{
			ATeleporter* T=Cast<ATeleporter>(Level->Actors(i));
			if( !T || appStrnicmp(T->GetName(),"DCSplitSky",10) ) continue;
			const char* Suffix=appStrstr(T->GetName(),"Exit");
			if( Suffix ) Suffix+=4;
			else { Suffix=appStrstr(T->GetName(),"Entry"); if(Suffix) Suffix+=5; }
			if( !Suffix || SkyTravelOpening(atoi(Suffix)) ) continue;
			printf("SPLITTRAVEL removed map=%s actor=%s reason=non-walk-opening-or-fragment\n",Map,T->GetName());
			T->bDeleteMe=1; Level->Actors(i)=NULL;
		}
	for( INT i=0; i<Level->Num(); ++i )
	{
		ATeleporter* Entry=Cast<ATeleporter>(Level->Actors(i));
		if( !Entry || appStrnicmp(Entry->GetName(),"DCSplit",7) || Entry->URL[0]
			|| SplitArrivalClear(Level,Entry->Location) ) continue;
		const FVector Old=Entry->Location;
		const INT Zone=Level->Model->PointRegion(Level->GetLevelInfo(),Old).ZoneNumber;
		UBOOL Found=0;
		// Stay in the same zone. Check the path from the old marker as a ray:
		// old generated markers can already clip the floor with a full cylinder.
		// The destination must fit the full cylinder, and cannot cross BSP walls.
		for( INT Radius=16; Radius<=384 && !Found; Radius+=16 )
			for( INT Direction=0; Direction<18 && !Found; ++Direction )
			{
				const FLOAT Angle=Direction*(2.f*PI/16.f);
				const FVector P=Old+(Direction<16 ? FVector(appCos(Angle)*Radius,appSin(Angle)*Radius,0)
					: FVector(0,0,Direction==16 ? Radius : -Radius));
				FCheckResult Hit(1.f), Point(1.f);
				if( !SplitArrivalClear(Level,P)
					|| Level->Model->PointRegion(Level->GetLevelInfo(),P).ZoneNumber!=Zone
					|| !Level->Model->PointCheck(Point,NULL,P,FVector(32,32,48),0)
					|| !Level->Model->LineCheck(Hit,NULL,P,Old,FVector(0,0,0),0) ) continue;
				Entry->Location=Entry->OldLocation=P;
				Entry->Region=Level->Model->PointRegion(Level->GetLevelInfo(),P);
				Found=1;
				printf("SPLITTRAVEL moved map=%s actor=%s from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f\n",
					Map,Entry->GetName(),Old.X,Old.Y,Old.Z,P.X,P.Y,P.Z);
			}
		if( !Found ) appErrorf("SPLITTRAVEL unsafe arrival %s.%s: no clear same-zone location",Map,Entry->GetName());
	}
	printf("SPLITTRAVEL arrivals_verified map=%s\n",Map);
}

void FDCUtil::FixSplitTravel( const char* MapPath, const char* OutPath )
{
	guard(FDCUtil::FixSplitTravel);
	if( !appStricmp(MapPath,OutPath) ) appErrorf("FIXSPLITTRAVEL needs a separate output");
	GIsEditor=false; GIsClient=true; GIsServer=true;
	ULevel* Level=LoadObject<ULevel>(NULL,"MyLevel",MapPath,LOAD_KeepImports|LOAD_NoFail,NULL);
	RepairSplitTravel(Level);
	if( !GObj.SavePackage(Level->GetParent(),Level,0,OutPath) ) appErrorf("FIXSPLITTRAVEL save failed");
	printf("FIXSPLITTRAVEL OK output=%s\n",OutPath);
	unguard;
}

static void AddSplitTeleporter( ULevel* Level, const char* Name, const char* Tag,
	const char* URL, FVector Location, FLOAT Radius, FLOAT Height, QWORD KeepMask )
{
	const INT Zone = Level->Model->PointRegion(Level->GetLevelInfo(), Location).ZoneNumber;
	if( Zone<=0 || !((KeepMask >> Zone) & 1) )
		appErrorf( "TESTSPLIT teleporter %s is outside retained zones (%d)", Name, Zone );
	ATeleporter* Teleporter = (ATeleporter*)GObj.ConstructObject(
		ATeleporter::StaticClass, Level->GetParent(), FName(Name), RF_Transactional,
		ATeleporter::StaticClass->GetDefaultActor());
	if( !Teleporter ) appErrorf( "TESTSPLIT could not create teleporter %s", Name );
	Teleporter->XLevel = Level;
	Teleporter->Level = Level->GetLevelInfo();
	Teleporter->Location = Teleporter->OldLocation = Location;
	Teleporter->Tag = FName(Tag);
	Teleporter->Region = Level->Model->PointRegion(Level->GetLevelInfo(), Location);
	Teleporter->CollisionRadius = Radius;
	Teleporter->CollisionHeight = Height;
	Teleporter->bEnabled = URL && *URL;
	appStrncpy(Teleporter->URL, URL ? URL : "", ARRAY_COUNT(Teleporter->URL));
	Level->Actors(Level->Add()) = Teleporter;
	printf( "TESTSPLIT teleporter %s zone=%d at=%.0f,%.0f,%.0f radius=%.0f url=%s\n",
		Name, Zone, Location.X, Location.Y, Location.Z, Radius, Teleporter->URL );
}

static void AddSplitPlayerStart( ULevel* Level, const char* Name, FVector Location, QWORD KeepMask )
{
	const INT Zone = Level->Model->PointRegion(Level->GetLevelInfo(), Location).ZoneNumber;
	if( Zone<=0 || !((KeepMask >> Zone) & 1) )
		appErrorf( "TESTSPLIT player start %s is outside retained zones (%d)", Name, Zone );
	APlayerStart* Start = (APlayerStart*)GObj.ConstructObject(
		APlayerStart::StaticClass, Level->GetParent(), FName(Name), RF_Transactional,
		APlayerStart::StaticClass->GetDefaultActor());
	Start->XLevel=Level;
	Start->Level=Level->GetLevelInfo();
	Start->Location=Start->OldLocation=Location;
	Start->Region=Level->Model->PointRegion(Level->GetLevelInfo(), Location);
	Start->bSinglePlayerStart=1;
	Level->Actors(Level->Add())=Start;
	printf( "TESTSPLIT player start %s zone=%d at=%.0f,%.0f,%.0f\n",
		Name, Zone, Location.X, Location.Y, Location.Z );
}

static void AddSplitEventRelay( ULevel* Level, const char* Prefix,
	const char* Event, FVector Location, QWORD KeepMask )
{
	const INT Zone=Level->Model->PointRegion(Level->GetLevelInfo(),Location).ZoneNumber;
	if( Zone<=0 || !((KeepMask >> Zone)&1) )
		appErrorf("TESTSPLIT relay %s outside retained zone %d",Event,Zone);
	char Name[64];
	snprintf(Name,sizeof(Name),"%s%s",Prefix,Event);
	ATrigger* Relay=(ATrigger*)GObj.ConstructObject(ATrigger::StaticClass,Level->GetParent(),
		FName(Name),RF_Transactional,ATrigger::StaticClass->GetDefaultActor());
	Relay->XLevel=Level;
	Relay->Level=Level->GetLevelInfo();
	Relay->Location=Relay->OldLocation=Location;
	Relay->Region=Level->Model->PointRegion(Level->GetLevelInfo(),Location);
	Relay->Tag=FName(Event);
	Relay->bHidden=1;
	Relay->bCollideActors=0;
	Relay->bBlockActors=0;
	Relay->bBlockPlayers=0;
	Level->Actors(Level->Add())=Relay;
	printf("TESTSPLIT relay event=%s zone=%d\n",Event,Zone);
}

static FVector AddRuinsSeam( ULevel* Level, const char* Role, INT Seam,
	FVector Center, FVector Normal, const INT* Heights, INT HeightCount, QWORD KeepMask )
{
	const INT PlusZone = Level->Model->PointRegion(Level->GetLevelInfo(), Center + Normal*120).ZoneNumber;
	const INT MinusZone = Level->Model->PointRegion(Level->GetLevelInfo(), Center - Normal*120).ZoneNumber;
	const UBOOL PlusKept = PlusZone>0 && ((KeepMask >> PlusZone) & 1);
	const UBOOL MinusKept = MinusZone>0 && ((KeepMask >> MinusZone) & 1);
	printf("TESTSPLIT Ruins seam=%d zones plus=%d minus=%d selected=%d/%d\n",
		Seam, PlusZone, MinusZone, PlusKept, MinusKept);
	if( PlusKept==MinusKept )
		appErrorf("TESTSPLIT Ruins seam %d does not separate selected zones",Seam);
	const FLOAT Direction = PlusKept ? 1.f : -1.f;
	const UBOOL PartOne = !appStricmp(Role,"Ruins1");
	char EntryName[64], EntryTag[64], ExitTag[64], Destination[128];
	snprintf(EntryName,sizeof(EntryName),"DCSplitRuins%dEntry%d",PartOne ? 1 : 2,Seam);
	snprintf(EntryTag,sizeof(EntryTag),"DCSplitRuins%dEntry%d",PartOne ? 1 : 2,Seam);
	snprintf(ExitTag,sizeof(ExitTag),"DCSplitRuins%dExit%d",PartOne ? 1 : 2,Seam);
	snprintf(Destination,sizeof(Destination),"Ruins%d#DCSplitRuins%dEntry%d?peer",
		PartOne ? 2 : 1, PartOne ? 2 : 1, Seam);
	for( INT i=0; i<HeightCount; ++i )
	{
		char ExitName[64];
		snprintf(ExitName,sizeof(ExitName),"DCSplitRuins%dExit%d_%d",
			PartOne ? 1 : 2,Seam,i);
		FVector Trigger = Center + Normal*(Direction*45.f);
		Trigger.Z = Heights[i];
		AddSplitTeleporter(Level, ExitName, ExitTag, Destination,
			Trigger, 150, 120, KeepMask);
	}
	const FVector Arrival = Center + Normal*(Direction*250.f);
	AddSplitTeleporter(Level, EntryName, EntryTag, "", Arrival, 0, 0, KeepMask);
	return Arrival;
}

static FVector AddChizraSeam( ULevel* Level, const char* Role, INT Seam,
	FVector Center, FVector Normal, const INT* Heights, INT HeightCount, FLOAT Radius,
	QWORD KeepMask )
{
	const INT PlusZone = Level->Model->PointRegion(Level->GetLevelInfo(),Center+Normal*120).ZoneNumber;
	const INT MinusZone = Level->Model->PointRegion(Level->GetLevelInfo(),Center-Normal*120).ZoneNumber;
	const UBOOL PlusKept = PlusZone>0 && ((KeepMask >> PlusZone) & 1);
	const UBOOL MinusKept = MinusZone>0 && ((KeepMask >> MinusZone) & 1);
	printf("TESTSPLIT Chizra seam=%d zones plus=%d minus=%d selected=%d/%d\n",
		Seam,PlusZone,MinusZone,PlusKept,MinusKept);
	if( PlusKept==MinusKept )
		appErrorf("TESTSPLIT Chizra seam %d does not divide selected zones",Seam);
	const FLOAT Direction = PlusKept ? 1.f : -1.f;
	const UBOOL PartOne = !appStricmp(Role,"Chizra1");
	char EntryName[64], EntryTag[64], ExitTag[64], Destination[128];
	snprintf(EntryName,sizeof(EntryName),"DCSplitChizra%dEntry%d",PartOne ? 1 : 2,Seam);
	snprintf(EntryTag,sizeof(EntryTag),"DCSplitChizra%dEntry%d",PartOne ? 1 : 2,Seam);
	snprintf(ExitTag,sizeof(ExitTag),"DCSplitChizra%dExit%d",PartOne ? 1 : 2,Seam);
	snprintf(Destination,sizeof(Destination),"Chizra%d#DCSplitChizra%dEntry%d?peer",
		PartOne ? 2 : 1,PartOne ? 2 : 1,Seam);
	for( INT i=0; i<HeightCount; ++i )
	{
		char ExitName[64];
		snprintf(ExitName,sizeof(ExitName),"DCSplitChizra%dExit%d_%d",
			PartOne ? 1 : 2,Seam,i);
		FVector Trigger = Center + Normal*(Direction*45.f);
		Trigger.Z = Heights[i];
		AddSplitTeleporter(Level,ExitName,ExitTag,Destination,Trigger,Radius,
			Seam==0 ? 150 : Seam==1 ? 200 : 82,KeepMask);
	}
	// The zone-21 side of seam 2 bends immediately behind the doorway; a
	// straight 245-unit offset lands inside CSG instead of the corridor.
	const FVector Arrival = Seam==2
		? (PartOne ? FVector(-6392,-3048,85) : FVector(-6779,-2833,85))
		: Center + Normal*(Direction*245.f);
	AddSplitTeleporter(Level,EntryName,EntryTag,"",Arrival,0,0,KeepMask);
	return Arrival;
}

static FVector AddTerraniuxSeam( ULevel* Level, const char* Role, INT Seam,
	FVector Center, INT ZoneA, INT ZoneB, FLOAT Radius, QWORD KeepMask )
{
	const UBOOL PartOne=!appStricmp(Role,"Terraniux1");
	const FVector Normal(0,1,0);
	// Pruning deliberately changes classification beyond the cut, so validate
	// only the selected side; the opposite probe may no longer name its zone.
	const INT Expected=((KeepMask >> ZoneA)&1) ? ZoneA : ZoneB;
	const INT Plus=Level->Model->PointRegion(Level->GetLevelInfo(),Center+Normal*96.f).ZoneNumber;
	const INT Minus=Level->Model->PointRegion(Level->GetLevelInfo(),Center-Normal*96.f).ZoneNumber;
	const FLOAT Direction=Plus==Expected && Minus!=Expected ? 1.f
		: Minus==Expected && Plus!=Expected ? -1.f : 0.f;
	if( !Direction )
		appErrorf("TESTSPLIT Terraniux seam %d expected %d got %d/%d",
			Seam,Expected,Plus,Minus);
	char Name[64], Tag[64], Destination[128];
	snprintf(Name,sizeof(Name),"DCSplitTerraniux%dExit%d",PartOne ? 1 : 2,Seam);
	snprintf(Tag,sizeof(Tag),"DCSplitTerraniux%dExit%d",PartOne ? 1 : 2,Seam);
	snprintf(Destination,sizeof(Destination),"Terraniux%d#DCSplitTerraniux%dEntry%d?peer",
		PartOne ? 2 : 1,PartOne ? 2 : 1,Seam);
	AddSplitTeleporter(Level,Name,Tag,Destination,Center+Normal*(Direction*48.f),Radius,120,KeepMask);
	snprintf(Name,sizeof(Name),"DCSplitTerraniux%dEntry%d",PartOne ? 1 : 2,Seam);
	snprintf(Tag,sizeof(Tag),"DCSplitTerraniux%dEntry%d",PartOne ? 1 : 2,Seam);
	// The exit trigger is 48 units from the seam with a 128-unit radius.
	// At 192, the arriving player's collision cylinder overlaps the reverse
	// trigger and causes endless Terraniux1 <-> Terraniux2 session travel.
	const FVector Arrival=Center+Normal*(Direction*256.f);
	AddSplitTeleporter(Level,Name,Tag,"",Arrival,0,0,KeepMask);
	printf("TESTSPLIT Terraniux seam=%d zones=%d/%d arrival=%.0f %.0f %.0f\n",
		Seam,ZoneA,ZoneB,Arrival.X,Arrival.Y,Arrival.Z);
	return Arrival;
}

static FVector AddIsvKran32Seam( ULevel* Level, const char* Role, QWORD KeepMask )
{
	// The only portal between the power-core spur (zone 21) and the rest
	// of the ship (zone 19) consists of two polygons in the X=1836 plane.
	const FVector Center(1836,-5104,-1094), Normal(1,0,0);
	const INT Plus=Level->Model->PointRegion(Level->GetLevelInfo(),Center+Normal*96.f).ZoneNumber;
	const INT Minus=Level->Model->PointRegion(Level->GetLevelInfo(),Center-Normal*96.f).ZoneNumber;
	const UBOOL PlusKept=Plus>0 && ((KeepMask >> Plus)&1);
	const UBOOL MinusKept=Minus>0 && ((KeepMask >> Minus)&1);
	if( PlusKept==MinusKept || (Plus!=19 && Minus!=19) || (Plus!=21 && Minus!=21) )
		appErrorf("TESTSPLIT IsvKran32 seam zones %d/%d selected %d/%d",
			Plus,Minus,PlusKept,MinusKept);
	const FLOAT Direction=PlusKept ? 1.f : -1.f;
	const UBOOL Main=!appStricmp(Role,"IsvKran32A");
	char Name[64], Tag[64], Destination[128];
	snprintf(Name,sizeof(Name),"DCSplitKran32%cExit",Main ? 'A' : 'B');
	snprintf(Tag,sizeof(Tag),"DCSplitKran32%cExit",Main ? 'A' : 'B');
	snprintf(Destination,sizeof(Destination),"IsvKran32%c#DCSplitKran32%cEntry?peer",
		Main ? 'B' : 'A',Main ? 'B' : 'A');
	AddSplitTeleporter(Level,Name,Tag,Destination,Center+Normal*(Direction*48.f),112,112,KeepMask);
	snprintf(Name,sizeof(Name),"DCSplitKran32%cEntry",Main ? 'A' : 'B');
	snprintf(Tag,sizeof(Tag),"DCSplitKran32%cEntry",Main ? 'A' : 'B');
	const FVector Arrival=Center+Normal*(Direction*256.f);
	AddSplitTeleporter(Level,Name,Tag,"",Arrival,0,0,KeepMask);
	printf("TESTSPLIT IsvKran32 seam=%d/%d part=%c arrival=%.0f %.0f %.0f\n",
		Plus,Minus,Main ? 'A' : 'B',Arrival.X,Arrival.Y,Arrival.Z);
	return Arrival;
}

void FDCUtil::TestSplitBsp( const char* MapPath, const char* OutPath, const char* Zones, const char* Role )
{
	guard(FDCUtil::TestSplitBsp);
	if( !appStricmp(MapPath, OutPath) )
		appErrorf( "TESTSPLIT output must differ from source" );
	GIsEditor = false;
	GIsClient = true;
	GIsServer = true;
	ULevel* Level = LoadObject<ULevel>( NULL, "MyLevel", MapPath, LOAD_KeepImports | LOAD_NoFail, NULL );
	check(Level && Level->Model && Level->Model->Nodes && Level->Model->Surfs
		&& Level->Model->Verts && Level->Model->Points && Level->Model->Vectors);
	UModel* Model = Level->Model;
	QWORD KeepMask = 0;
	const char* Cursor = Zones;
	while( *Cursor )
	{
		char* End = NULL;
		long Zone = strtol( Cursor, &End, 10 );
		if( End == Cursor || Zone <= 0 || Zone >= Model->Nodes->NumZones )
			appErrorf( "TESTSPLIT invalid KEEPZONES near '%s'", Cursor );
		KeepMask |= (QWORD)1 << Zone;
		if( !*End ) break;
		if( *End != '+' )
			appErrorf( "TESTSPLIT expected '+' near '%s'", End );
		Cursor = End + 1;
	}
	if( !KeepMask || !Model->Nodes->Num() )
		appErrorf( "TESTSPLIT empty zone selection or BSP" );

	const INT OldNodes = Model->Nodes->Num();
	const INT OldSurfs = Model->Surfs->Num();
	const INT OldVerts = Model->Verts->Num();
	const INT OldPoints = Model->Points->Num();
	const INT OldVectors = Model->Vectors->Num();
	TArray<BYTE> Reachable, Direct, Keep;
	Reachable.AddZeroed(OldNodes);
	Direct.AddZeroed(OldNodes);
	Keep.AddZeroed(OldNodes);
	TArray<INT> Parent, Stack;
	Parent.Add(OldNodes);
	for( INT i=0; i<OldNodes; ++i ) Parent(i) = INDEX_NONE;
	Stack.AddItem(0);
	while( Stack.Num() )
	{
		const INT i = Stack(Stack.Num()-1);
		Stack.Remove(Stack.Num()-1);
		if( Reachable(i) ) continue;
		Reachable(i) = 1;
		const FBspNode& Node = Model->Nodes->Element(i);
		for( INT c=0; c<3; ++c )
		{
			const INT Child = Node.iChild[c];
			if( Child == INDEX_NONE ) continue;
			if( Child < 0 || Child >= OldNodes )
				appErrorf( "TESTSPLIT invalid node child %d", Child );
			if( Parent(Child) == INDEX_NONE && Child != 0 ) Parent(Child) = i;
			if( !Reachable(Child) ) Stack.AddItem(Child);
		}
	}
	for( INT i=0; i<OldNodes; ++i )
	{
		if( !Reachable(i) ) continue;
		const FBspNode& Node = Model->Nodes->Element(i);
		const QWORD Sides = (Node.iZone[0] < 64 ? (QWORD)1 << Node.iZone[0] : 0)
			| (Node.iZone[1] < 64 ? (QWORD)1 << Node.iZone[1] : 0);
		if( !Node.NumVertices || !(Sides & KeepMask) ) continue;
		Direct(i) = 1;
		for( INT Walk=i, Steps=0; Walk != INDEX_NONE; Walk=Parent(Walk) )
		{
			if( ++Steps > OldNodes ) appErrorf( "TESTSPLIT cyclic BSP parent links" );
			Keep(Walk) = 1;
		}
	}
	if( !Keep(0) ) appErrorf( "TESTSPLIT no BSP polygons in selected zones" );
	// Most splits retain the complete CSG tree. Terraniux experiments prune
	// branches with no selected-zone polygon; the region and ray probes below
	// reject a candidate if doing so changes collision in the retained half.
	const UBOOL PruneBsp = Role && (!appStricmp(Role,"Terraniux1") || !appStricmp(Role,"Terraniux2")
		|| !appStricmp(Role,"IsvKran32A") || !appStricmp(Role,"IsvKran32B")
		|| !appStricmp(Role,"SkyTown1") || !appStricmp(Role,"SkyTown2"));
	if( !PruneBsp )
		for( INT i=0; i<OldNodes; ++i )
			if( Reachable(i) ) Keep(i) = 1;

	// Baseline point-region checks catch accidental CSG/tree changes inside the
	// retained half before the package is written.
	TArray<INT> ActorZones;
	ActorZones.Add(Level->Num());
	TArray<BYTE> ActorRayClear;
	TArray<FLOAT> ActorRayTime;
	const FVector RayOffset[] = {
		FVector(128,0,0), FVector(-128,0,0), FVector(0,128,0), FVector(0,-128,0),
		FVector(256,0,0), FVector(-256,0,0), FVector(0,256,0), FVector(0,-256,0),
		FVector(128,128,0), FVector(-128,128,0),
		FVector(0,0,128), FVector(0,0,-128)
	};
	const INT RayCount=PruneBsp ? ARRAY_COUNT(RayOffset) : 4;
	ActorRayClear.Add(Level->Num() * RayCount);
	ActorRayTime.Add(Level->Num() * RayCount);
	for( INT i=0; i<Level->Num(); ++i )
	{
		ActorZones(i) = Level->Actors(i)
			? Model->PointRegion(Level->GetLevelInfo(), Level->Actors(i)->Location).ZoneNumber
			: INDEX_NONE;
		if( ActorZones(i) <= 0 || !((KeepMask >> ActorZones(i)) & 1) ) continue;
		for( INT Ray=0; Ray<RayCount; ++Ray )
		{
			FCheckResult Hit(1.0f);
			ActorRayClear(i*RayCount+Ray) = Model->LineCheck(Hit, NULL,
				Level->Actors(i)->Location + RayOffset[Ray], Level->Actors(i)->Location,
				FVector(0,0,0), 0);
			ActorRayTime(i*RayCount+Ray) = Hit.Time;
			if( PruneBsp )
				KeepSplitCollisionLine(Model, 0, Level->Actors(i)->Location,
					Level->Actors(i)->Location + RayOffset[Ray], Keep);
		}
	}

	// Retained collision hulls reference BSP planes by node index, including
	// planes outside the retained render subtree. Keep those as plane-only
	// orphans so the original hulls can be remapped without a CSG rebuild.
	TArray<BYTE> HullOnly;
	HullOnly.AddZeroed(OldNodes);
	for( INT i=0; i<OldNodes; ++i )
	{
		if( !Keep(i) ) continue;
		const INT Bound = Model->Nodes->Element(i).iCollisionBound;
		if( Bound == INDEX_NONE ) continue;
		if( Bound < 0 || Bound >= Model->LeafHulls.Num() )
			appErrorf( "TESTSPLIT invalid collision bound in node %d", i );
		for( INT j=Bound; j<Model->LeafHulls.Num(); ++j )
		{
			const INT Entry = Model->LeafHulls(j);
			if( Entry == INDEX_NONE ) break;
			const INT HullNode = Entry & ~0x40000000;
			if( HullNode < 0 || HullNode >= OldNodes )
				appErrorf( "TESTSPLIT invalid hull node %d", HullNode );
			if( !Keep(HullNode) ) HullOnly(HullNode) = 1;
			if( j-Bound > 128 ) appErrorf( "TESTSPLIT unterminated collision hull" );
		}
	}
	TArray<INT> NodeRemap, SurfRemap, PointRemap, VectorRemap;
	NodeRemap.Add(OldNodes); SurfRemap.Add(OldSurfs);
	PointRemap.Add(OldPoints); VectorRemap.Add(OldVectors);
	for( INT i=0; i<OldNodes; ++i ) NodeRemap(i) = INDEX_NONE;
	for( INT i=0; i<OldSurfs; ++i ) SurfRemap(i) = INDEX_NONE;
	for( INT i=0; i<OldPoints; ++i ) PointRemap(i) = INDEX_NONE;
	for( INT i=0; i<OldVectors; ++i ) VectorRemap(i) = INDEX_NONE;
	INT ReachCount=0, DirectCount=0;
	for( INT i=0; i<OldNodes; ++i )
	{
		ReachCount += Reachable(i) != 0;
		DirectCount += Direct(i) != 0;
	}
	TArray<FBspNode> PackedNodes;
	TArray<INT> PackedOldNodes;
	TArray<FVert> PackedVerts;
	for( INT i=0; i<OldNodes; ++i )
	{
		if( !Keep(i) && !HullOnly(i) ) continue;
		FBspNode Node = Model->Nodes->Element(i);
		NodeRemap(i) = PackedNodes.Num();
		if( Direct(i) )
		{
			if( Node.iVertPool < 0 || Node.iVertPool + Node.NumVertices > OldVerts )
				appErrorf( "TESTSPLIT invalid vertex pool in node %d", i );
			const INT OldStart = Node.iVertPool;
			Node.iVertPool = PackedVerts.Num();
			for( INT j=0; j<Node.NumVertices; ++j )
				PackedVerts.AddItem(Model->Verts->Element(OldStart+j));
		}
		else
		{
			if( Node.IsCsg() ) Node.NodeFlags |= NF_DC_CsgOnly;
			Node.NumVertices = 0;
			Node.iVertPool = INDEX_NONE;
			Node.iSurf = INDEX_NONE;
		}
		Node.ZoneMask &= KeepMask;
		if( HullOnly(i) )
		{
			Node.iSurf = INDEX_NONE;
			Node.iChild[0] = Node.iChild[1] = Node.iChild[2] = INDEX_NONE;
			Node.iCollisionBound = Node.iRenderBound = INDEX_NONE;
			Node.iLeaf[0] = Node.iLeaf[1] = INDEX_NONE;
		}
		PackedNodes.AddItem(Node);
		PackedOldNodes.AddItem(i);
	}
	TArray<INT> HullRemap, PackedHulls;
	HullRemap.Add(Model->LeafHulls.Num());
	for( INT i=0; i<HullRemap.Num(); ++i ) HullRemap(i) = INDEX_NONE;
	for( INT i=0; i<PackedNodes.Num(); ++i )
	{
		FBspNode& Node = PackedNodes(i);
		if( !Keep(PackedOldNodes(i)) ) continue;
		const INT OldBound = Node.iCollisionBound;
		if( OldBound == INDEX_NONE ) continue;
		if( HullRemap(OldBound) == INDEX_NONE )
		{
			HullRemap(OldBound) = PackedHulls.Num();
			INT j = OldBound;
			for( ; j<Model->LeafHulls.Num() && Model->LeafHulls(j)!=INDEX_NONE; ++j )
			{
				const INT Entry = Model->LeafHulls(j);
				PackedHulls.AddItem(SplitIndex(NodeRemap, Entry & ~0x40000000, "hull node")
					| (Entry & 0x40000000));
			}
			if( j+6 >= Model->LeafHulls.Num() )
				appErrorf( "TESTSPLIT truncated collision hull" );
			for( INT k=0; k<7; ++k ) PackedHulls.AddItem(Model->LeafHulls(j+k));
		}
		Node.iCollisionBound = HullRemap(OldBound);
	}
	TArray<FBspSurf> PackedSurfs;
	for( INT i=0; i<PackedNodes.Num(); ++i )
	{
		FBspNode& Node = PackedNodes(i);
		for( INT c=0; c<3; ++c )
		{
			const INT OldChild = Node.iChild[c];
			Node.iChild[c] = OldChild != INDEX_NONE && OldChild >= 0 && OldChild < OldNodes
				&& Keep(OldChild) ? NodeRemap(OldChild) : INDEX_NONE;
		}
		if( Node.iSurf == INDEX_NONE ) continue;
		if( Node.iSurf < 0 || Node.iSurf >= OldSurfs )
			appErrorf( "TESTSPLIT invalid surface in packed node %d", i );
		if( SurfRemap(Node.iSurf) == INDEX_NONE )
			SurfRemap(Node.iSurf) = PackedSurfs.AddItem(Model->Surfs->Element(Node.iSurf));
		Node.iSurf = SurfRemap(Node.iSurf);
	}
	TArray<FVector> PackedPoints, PackedVectors;
	for( INT i=0; i<PackedVerts.Num(); ++i )
		PackedVerts(i).pVertex = SplitPoint(Model, PackedVerts(i).pVertex, PointRemap, PackedPoints);
	for( INT i=0; i<PackedSurfs.Num(); ++i )
	{
		FBspSurf& Surf = PackedSurfs(i);
		Surf.pBase = SplitPoint(Model, Surf.pBase, PointRemap, PackedPoints);
		Surf.vNormal = SplitVector(Model, Surf.vNormal, VectorRemap, PackedVectors);
		Surf.vTextureU = SplitVector(Model, Surf.vTextureU, VectorRemap, PackedVectors);
		Surf.vTextureV = SplitVector(Model, Surf.vTextureV, VectorRemap, PackedVectors);
	}

	Model->Nodes->SetNum(PackedNodes.Num()); Model->Nodes->SetMax(PackedNodes.Num()); Model->Nodes->Realloc();
	for( INT i=0; i<PackedNodes.Num(); ++i ) Model->Nodes->Element(i) = PackedNodes(i);
	Model->Surfs->SetNum(PackedSurfs.Num()); Model->Surfs->SetMax(PackedSurfs.Num()); Model->Surfs->Realloc();
	for( INT i=0; i<PackedSurfs.Num(); ++i ) Model->Surfs->Element(i) = PackedSurfs(i);
	Model->Verts->SetNum(PackedVerts.Num()); Model->Verts->SetMax(PackedVerts.Num()); Model->Verts->Realloc();
	for( INT i=0; i<PackedVerts.Num(); ++i ) Model->Verts->Element(i) = PackedVerts(i);
	Model->Points->SetNum(PackedPoints.Num()); Model->Points->SetMax(PackedPoints.Num()); Model->Points->Realloc();
	for( INT i=0; i<PackedPoints.Num(); ++i ) Model->Points->Element(i) = PackedPoints(i);
	Model->Vectors->SetNum(PackedVectors.Num()); Model->Vectors->SetMax(PackedVectors.Num()); Model->Vectors->Realloc();
	for( INT i=0; i<PackedVectors.Num(); ++i ) Model->Vectors->Element(i) = PackedVectors(i);
	Model->LeafHulls.Empty();
	for( INT i=0; i<PackedHulls.Num(); ++i ) Model->LeafHulls.AddItem(PackedHulls(i));
	Model->LeafHulls.Shrink();

	INT Checked=0, Mismatched=0, RayChecked=0, RayMismatched=0;
	for( INT i=0; i<Level->Num(); ++i )
	{
		AActor* Actor = Level->Actors(i);
		if( !Actor || ActorZones(i) <= 0 || !((KeepMask >> ActorZones(i)) & 1)) continue;
		++Checked;
		const INT NewZone = Model->PointRegion(Level->GetLevelInfo(), Actor->Location).ZoneNumber;
		if( NewZone != ActorZones(i) )
		{
			if( Mismatched < 12 )
				printf( "TESTSPLIT region mismatch actor=%s old=%d new=%d\n",
					Actor->GetName(), ActorZones(i), NewZone );
			++Mismatched;
		}
		for( INT Ray=0; Ray<RayCount; ++Ray )
		{
			FCheckResult Hit(1.0f);
			const UBOOL Clear = Model->LineCheck(Hit, NULL, Actor->Location + RayOffset[Ray],
				Actor->Location, FVector(0,0,0), 0);
			++RayChecked;
			if( Clear != ActorRayClear(i*RayCount+Ray)
				|| (!Clear && Abs(Hit.Time-ActorRayTime(i*RayCount+Ray)) > 0.01f) )
			{
				if( RayMismatched < 12 )
					printf( "TESTSPLIT ray mismatch actor=%s ray=%d old=%d/%.3f new=%d/%.3f\n",
						Actor->GetName(), Ray, ActorRayClear(i*RayCount+Ray), ActorRayTime(i*RayCount+Ray),
						Clear, Hit.Time );
				++RayMismatched;
			}
		}
	}
	printf( "TESTSPLIT geometry mask=%08x:%08x reachable=%d direct=%d nodes=%d/%d hullints=%d surfs=%d/%d verts=%d/%d points=%d/%d vectors=%d/%d region=%d/%d rays=%d/%d\n",
		(DWORD)(KeepMask >> 32), (DWORD)KeepMask, ReachCount, DirectCount,
		PackedNodes.Num(), OldNodes, PackedHulls.Num(), PackedSurfs.Num(), OldSurfs, PackedVerts.Num(), OldVerts,
		PackedPoints.Num(), OldPoints, PackedVectors.Num(), OldVectors, Checked-Mismatched, Checked,
		RayChecked-RayMismatched, RayChecked );
	fflush(stdout);
	if( Mismatched ) appErrorf( "TESTSPLIT changed %d retained actor regions", Mismatched );
	if( RayMismatched ) appErrorf( "TESTSPLIT changed %d retained actor collision rays", RayMismatched );

	// Keep mandatory level objects and zone-zero actors on both sides. Zone-zero
	// actors need a later ownership audit; dropping them blindly loses script
	// managers, sky actors and inventory that PointRegion cannot classify.
	TArray<BYTE> ActorKeep;
	ActorKeep.Add(Level->Num());
	INT ActorsKept=0, ActorsRemoved=0, ActorsZoneZero=0;
	for( INT i=0; i<Level->Num(); ++i )
	{
		AActor* Actor = Level->Actors(i);
		const INT Zone = ActorZones(i);
		ActorKeep(i) = Actor && (i<2 || Zone<=0 || ((KeepMask >> Zone) & 1));
		if( !Actor ) continue;
		if( Zone<=0 ) ++ActorsZoneZero;
		if( ActorKeep(i) ) ++ActorsKept;
		else { Actor->bDeleteMe=1; ++ActorsRemoved; }
	}
	// Clear reflected actor references in retained actors before disconnecting
	// excluded actors from ULevel's serialized actor array.
	for( INT i=0; i<Level->Num(); ++i )
		if( ActorKeep(i) )
			Level->Actors(i)->GetClass()->CleanupDestroyed((BYTE*)Level->Actors(i));
	for( INT i=0; i<Level->Num(); ++i )
		if( !ActorKeep(i) ) Level->Actors(i)=NULL;
	Level->FirstDeleted=NULL;
	for( INT i=0; i<Model->Nodes->NumZones; ++i )
		if( Model->Nodes->Zones[i].ZoneActor && Model->Nodes->Zones[i].ZoneActor->bDeleteMe )
			Model->Nodes->Zones[i].ZoneActor=Level->GetLevelInfo();
	for( INT i=0; i<Model->Surfs->Num(); ++i )
		if( Model->Surfs->Element(i).Actor && Model->Surfs->Element(i).Actor->bDeleteMe )
			Model->Surfs->Element(i).Actor=NULL;

	// UModel's baked light lists are not reflected actor properties, so
	// CleanupDestroyed above cannot remove their pointers to split-away actors.
	const TArray<AActor*> OldLights = Model->Lights;
	TArray<INT> LightRootRemap;
	LightRootRemap.Add(OldLights.Num());
	for( INT i=0; i<LightRootRemap.Num(); ++i ) LightRootRemap(i)=INDEX_NONE;
	TArray<AActor*> PackedLights;
	INT RemovedLightRefs=0;
	for( INT i=0; i<Model->LightMap.Num(); ++i )
		Model->LightMap(i).iLightActors = SplitLightRoot(OldLights,
			Model->LightMap(i).iLightActors,LightRootRemap,PackedLights,RemovedLightRefs);
	for( INT i=0; i<Model->Leaves.Num(); ++i )
	{
		Model->Leaves(i).iPermeating = SplitLightRoot(OldLights,
			Model->Leaves(i).iPermeating,LightRootRemap,PackedLights,RemovedLightRefs);
		Model->Leaves(i).iVolumetric = SplitLightRoot(OldLights,
			Model->Leaves(i).iVolumetric,LightRootRemap,PackedLights,RemovedLightRefs);
	}
	Model->Lights=PackedLights;
	Model->Lights.Shrink();
	INT LightRoots=0;
	for( INT i=0; i<LightRootRemap.Num(); ++i )
		if( LightRootRemap(i)!=INDEX_NONE ) ++LightRoots;
	printf("TESTSPLIT light lists entries=%d/%d roots=%d removed_refs=%d\n",
		Model->Lights.Num(),OldLights.Num(),LightRoots,RemovedLightRefs);

	TArray<INT> ReachRemap;
	ReachRemap.Add(Level->ReachSpecs.Num());
	for( INT i=0; i<ReachRemap.Num(); ++i ) ReachRemap(i)=INDEX_NONE;
	TArray<FReachSpec> PackedReach;
	for( INT i=0; i<Level->ReachSpecs.Num(); ++i )
	{
		const FReachSpec& Spec = Level->ReachSpecs(i);
		if( Spec.Start && Spec.End && !Spec.Start->bDeleteMe && !Spec.End->bDeleteMe )
			ReachRemap(i)=PackedReach.AddItem(Spec);
	}
	for( INT i=0; i<Level->Num(); ++i )
	{
		ANavigationPoint* Nav = Cast<ANavigationPoint>(Level->Actors(i));
		if( !Nav ) continue;
		INT* Arrays[3] = { Nav->Paths, Nav->upstreamPaths, Nav->PrunedPaths };
		for( INT a=0; a<3; ++a )
		{
			INT Write=0;
			for( INT j=0; j<16 && Arrays[a][j]!=INDEX_NONE; ++j )
			{
				const INT Old=Arrays[a][j];
				if( Old>=0 && Old<ReachRemap.Num() && ReachRemap(Old)!=INDEX_NONE )
					Arrays[a][Write++]=ReachRemap(Old);
			}
			while( Write<16 ) Arrays[a][Write++]=INDEX_NONE;
		}
	}
	Level->ReachSpecs=PackedReach;
	printf( "TESTSPLIT actors kept=%d removed=%d zone_zero=%d reach=%d/%d\n",
		ActorsKept, ActorsRemoved, ActorsZoneZero, PackedReach.Num(), ReachRemap.Num() );
	if( Role && !appStricmp(Role,"Dig1") )
	{
		AddSplitTeleporter(Level, "DCSplitDig1Exit0", "DCSplitDig1Exit", "Dig2#DCSplitDig2Entry?peer", FVector(1990,-55,-669), 80, 125, KeepMask);
		AddSplitTeleporter(Level, "DCSplitDig1Exit1", "DCSplitDig1Exit", "Dig2#DCSplitDig2Entry?peer", FVector(1990,100,-669), 80, 125, KeepMask);
		AddSplitTeleporter(Level, "DCSplitDig1Exit2", "DCSplitDig1Exit", "Dig2#DCSplitDig2Entry?peer", FVector(1990,255,-669), 80, 125, KeepMask);
		AddSplitTeleporter(Level, "DCSplitDig1Entry", "DCSplitDig1Entry", "", FVector(1860,47,-669), 0, 0, KeepMask);
	}
	else if( Role && !appStricmp(Role,"Dig2") )
	{
		// No generated reverse travel across Dig's forward-only cut.
		AddSplitTeleporter(Level, "DCSplitDig2Entry", "DCSplitDig2Entry", "", FVector(2250,47,-733), 0, 0, KeepMask);
		AddSplitPlayerStart(Level, "DCSplitDig2Start", FVector(2250,47,-733), KeepMask);
	}
	else if( Role && (!appStricmp(Role,"DasaCellars1") || !appStricmp(Role,"DasaCellars2")) )
	{
		// The 1<->6 portal is a 256x256 doorway in the Y=624 plane.
		// Probe both sides after the BSP remap so a bad seam placement fails the
		// cook, rather than creating a teleporter in the wrong room.
		const FVector FirstSide(7848,448,-300), SecondSide(7848,800,-300);
		const INT FirstZone = Model->PointRegion(Level->GetLevelInfo(), FirstSide).ZoneNumber;
		const INT SecondZone = Model->PointRegion(Level->GetLevelInfo(), SecondSide).ZoneNumber;
		printf("TESTSPLIT DasaCellars seam zones y448=%d y800=%d\n", FirstZone, SecondZone);
		if( FirstZone<=0 || SecondZone<=0 || FirstZone==SecondZone )
			appErrorf("TESTSPLIT DasaCellars seam does not divide two zones");
		const UBOOL OnFirstSide = (KeepMask >> FirstZone) & 1;
		const UBOOL OnSecondSide = (KeepMask >> SecondZone) & 1;
		if( OnFirstSide==OnSecondSide )
			appErrorf("TESTSPLIT DasaCellars seam mask does not select one side");
		const UBOOL PartOne = !appStricmp(Role,"DasaCellars1");
		if( PartOne )
			AddSplitTeleporter(Level, "DCSplitDasa1Exit", "DCSplitDasa1Exit",
				"DasaCellars2#DCSplitDasa2Entry?peer",
				FVector(7848,OnFirstSide ? 524 : 724,-300), 170, 140, KeepMask);
		else
		{
			// The zone-6 landing is only about 176 units deep. Three small
			// triggers cover the doorway without touching its landing marker.
			const INT LaneX[3] = { 7763, 7848, 7933 };
			for( INT Lane=0; Lane<3; ++Lane )
			{
				char Name[64];
				snprintf(Name,sizeof(Name),"DCSplitDasa2Exit%d",Lane);
				AddSplitTeleporter(Level, Name, "DCSplitDasa2Exit",
					"DasaCellars1#DCSplitDasa1Entry?peer",
					FVector(LaneX[Lane],OnFirstSide ? 573 : 675,-300), 88, 140, KeepMask);
			}
		}
		AddSplitTeleporter(Level, PartOne ? "DCSplitDasa1Entry" : "DCSplitDasa2Entry",
			PartOne ? "DCSplitDasa1Entry" : "DCSplitDasa2Entry", "",
			FVector(7848,PartOne ? (OnFirstSide ? 324 : 924)
				: (OnFirstSide ? 448 : 800),-300), 0, 0, KeepMask);
		if( !PartOne )
			AddSplitPlayerStart(Level, "DCSplitDasa2Start",
				FVector(7848,OnFirstSide ? 448 : 800,-300), KeepMask);
	}
	else if( Role && (!appStricmp(Role,"Ruins1") || !appStricmp(Role,"Ruins2")) )
	{
		const INT Heights0[2] = { 12, -188 };
		const INT Heights1[3] = { -156, -340, -498 };
		const INT Heights2[1] = { 128 };
		const FVector Arrival0 = AddRuinsSeam(Level,Role,0,
			FVector(2304,-4880,-88),FVector(0,1,0),Heights0,2,KeepMask);
		AddRuinsSeam(Level,Role,1,
			FVector(1272,-2309,-300),FVector(1,0,0),Heights1,3,KeepMask);
		AddRuinsSeam(Level,Role,2,
			FVector(-384,-2112,128),FVector(1,0,0),Heights2,1,KeepMask);
		if( !appStricmp(Role,"Ruins2") )
			AddSplitPlayerStart(Level,"DCSplitRuins2Start",Arrival0,KeepMask);
	}
	else if( Role && (!appStricmp(Role,"Chizra1") || !appStricmp(Role,"Chizra2")) )
	{
		const INT Heights0[1] = { 270 };
		const INT Heights1[1] = { 224 };
		const INT Heights2[2] = { 96, 224 };
		const FVector Arrival0 = AddChizraSeam(Level,Role,0,
			FVector(-6136,-2576,270),FVector(0,1,0),Heights0,1,145,KeepMask);
		AddChizraSeam(Level,Role,1,
			FVector(-6392,-3440,224),FVector(0,1,0),Heights1,1,90,KeepMask);
		AddChizraSeam(Level,Role,2,
			FVector(-6528,-2800,160),FVector(1,0,0),Heights2,2,145,KeepMask);
		if( !appStricmp(Role,"Chizra2") )
		{
			for( INT i=0; i<Level->Num(); ++i )
			{
				ATeleporter* Teleporter = Cast<ATeleporter>(Level->Actors(i));
				if( Teleporter && !appStricmp(Teleporter->URL,"warp") )
				{
					appStrncpy(Teleporter->URL,"Chizra1#Warp?peer",ARRAY_COUNT(Teleporter->URL));
					printf("TESTSPLIT Chizra warp actor=%s url=%s\n",
						Teleporter->GetName(),Teleporter->URL);
				}
			}
			AddSplitPlayerStart(Level,"DCSplitChizra2Start",Arrival0,KeepMask);
		}
	}
	else if( Role && (!appStricmp(Role,"Terraniux1") || !appStricmp(Role,"Terraniux2")) )
	{
		AddTerraniuxSeam(Level,Role,0,FVector(-1008,-14256,-88),1,2,128,KeepMask);
		AddTerraniuxSeam(Level,Role,1,FVector(720,-14704,1408),27,28,128,KeepMask);
		const FVector Arrival=AddTerraniuxSeam(Level,Role,2,
			FVector(-952,-14848,1536),5,12,128,KeepMask);
		if( !appStricmp(Role,"Terraniux2") )
			AddSplitPlayerStart(Level,"DCSplitTerraniux2Start",Arrival,KeepMask);
		const UBOOL PartOne=!appStricmp(Role,"Terraniux1");
		const char* Events1[] = {"lickmer","fast","sli","goh","sluty","tremor","tremor2","noback"};
		const char* Events2[] = {"fast"};
		const char** Events=PartOne ? Events1 : Events2;
		const INT Count=PartOne ? ARRAY_COUNT(Events1) : ARRAY_COUNT(Events2);
		const FVector RelayLocation=PartOne ? FVector(-1008,-14064,-88) : FVector(-1008,-14448,-88);
		for( INT i=0; i<Count; ++i ) AddSplitEventRelay(Level,
			"DCSplitTerraniuxRelay_",Events[i],RelayLocation,KeepMask);
	}
	else if( Role && (!appStricmp(Role,"IsvKran32A") || !appStricmp(Role,"IsvKran32B")) )
	{
		const FVector Arrival=AddIsvKran32Seam(Level,Role,KeepMask);
		if( !appStricmp(Role,"IsvKran32B") )
			AddSplitPlayerStart(Level,"DCSplitKran32BStart",Arrival,KeepMask);
	}
	else if( Role && (!appStricmp(Role,"SkyTown1") || !appStricmp(Role,"SkyTown2")) )
	{
		const UBOOL Main=!appStricmp(Role,"SkyTown1");
		INT Portals=0;
		for( INT i=0; i<PackedNodes.Num(); ++i )
		{
			const FBspNode& Node=PackedNodes(i);
			const INT A=Node.iZone[0], B=Node.iZone[1];
			const UBOOL Crossing=(A==2 && (B==6 || B==24))
				|| (B==2 && (A==6 || A==24))
				|| (A==8 && (B==6 || B==7))
				|| (B==8 && (A==6 || A==7))
				|| (A==28 && B==30) || (A==30 && B==28);
			if( !Crossing || Node.NumVertices<3 || !SkyTravelOpening(PackedOldNodes(i)) ) continue;
			FVector Center(0,0,0);
			for( INT j=0; j<Node.NumVertices; ++j )
				Center += Model->Points->Element(Model->Verts->Element(Node.iVertPool+j).pVertex);
			Center /= (FLOAT)Node.NumVertices;
			const FVector Normal(Node.Plane.X,Node.Plane.Y,Node.Plane.Z);
			const INT Plus=Model->PointRegion(Level->GetLevelInfo(),Center+Normal*96.f).ZoneNumber;
			const INT Minus=Model->PointRegion(Level->GetLevelInfo(),Center-Normal*96.f).ZoneNumber;
			const UBOOL PlusKept=Plus>0 && ((KeepMask >> Plus)&1);
			const UBOOL MinusKept=Minus>0 && ((KeepMask >> Minus)&1);
			if( PlusKept==MinusKept )
				appErrorf("TESTSPLIT Sky portal %d has ambiguous sides %d/%d",
					PackedOldNodes(i),Plus,Minus);
			const FLOAT Direction=PlusKept ? 1.f : -1.f;
			char Name[64], Tag[64], Destination[128];
			snprintf(Name,sizeof(Name),"DCSplitSky%dExit%d",Main ? 1 : 2,PackedOldNodes(i));
			snprintf(Tag,sizeof(Tag),"DCSplitSky%dExit",Main ? 1 : 2);
			snprintf(Destination,sizeof(Destination),"SkyTown%d#DCSplitSky%dEntry%d?peer",
				Main ? 2 : 1,Main ? 2 : 1,PackedOldNodes(i));
			AddSplitTeleporter(Level,Name,Tag,Destination,
				Center+Normal*(Direction*96.f),112,112,KeepMask);
			snprintf(Name,sizeof(Name),"DCSplitSky%dEntry%d",Main ? 1 : 2,PackedOldNodes(i));
			snprintf(Tag,sizeof(Tag),"DCSplitSky%dEntry%d",Main ? 1 : 2,PackedOldNodes(i));
			AddSplitTeleporter(Level,Name,Tag,"",Center+Normal*(Direction*256.f),0,0,KeepMask);
			++Portals;
		}
		if( Portals!=5 ) appErrorf("TESTSPLIT SkyTown expected five audited walk openings, found %d",Portals);
		if( !Main )
		{
			AddSplitPlayerStart(Level,"DCSplitSkyTown2Start",FVector(-1024,-638,2146),KeepMask);
			AddSplitEventRelay(Level,"DCSplitSkyTownRelay_","Barndoors3",
				FVector(-1024,-638,2146),KeepMask);
			AddSplitEventRelay(Level,"DCSplitSkyTownRelay_","StopAmbient",
				FVector(-192,-3520,1968),KeepMask);
		}
		printf("TESTSPLIT SkyTown part=%d portal_pairs=%d\n",Main ? 1 : 2,Portals);
	}
	else if( Role && *Role )
		appErrorf( "TESTSPLIT unknown travel role %s", Role );
	RepairSplitTravel(Level);
	if( !GObj.SavePackage(Level->GetParent(), Level, 0, OutPath) )
		appErrorf( "TESTSPLIT save failed" );
	printf( "TESTSPLIT OK output=%s\n", OutPath );
	unguard;
}
