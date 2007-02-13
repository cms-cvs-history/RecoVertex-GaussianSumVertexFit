#include "RecoVertex/GaussianSumVertexFit/interface/GsfVertexSmoother.h"
#include "RecoVertex/GaussianSumVertexFit/interface/BasicMultiVertexState.h"
#include "RecoVertex/GaussianSumVertexFit/interface/MultiRefittedTS.h"
#include "RecoVertex/VertexPrimitives/interface/VertexException.h"

GsfVertexSmoother::GsfVertexSmoother(bool limit, const GsfVertexMerger * merger) :
  limitComponents (limit), theMerger(merger->clone())
{}

CachingVertex
GsfVertexSmoother::smooth(const CachingVertex & vertex) const
{

  vector<RefCountedVertexTrack> tracks = vertex.tracks();
  int numberOfTracks = tracks.size();

  // Initial vertex for ascending fit
  GlobalPoint priorVertexPosition = tracks[0]->linearizedTrack()->linearizationPoint();
  AlgebraicSymMatrix we(3,1);
  GlobalError priorVertexError(we*10000);

  vector<RefCountedVertexTrack> initialTracks;
  CachingVertex fitVertex(priorVertexPosition,priorVertexError,initialTracks,0);
  //In case prior vertex was used.
  if (vertex.hasPrior()) {
    VertexState priorVertexState = vertex.priorVertexState();
    fitVertex = CachingVertex(priorVertexState, priorVertexState,
    		initialTracks,0);
  }

  // vertices from ascending fit
  vector<CachingVertex> ascendingFittedVertices;
  ascendingFittedVertices.reserve(numberOfTracks);
  ascendingFittedVertices.push_back(fitVertex);

  // ascending fit
  for (vector<RefCountedVertexTrack>::const_iterator i = tracks.begin();
	  i != (tracks.end()-1); ++i) {
    fitVertex = theUpdator.add(fitVertex,*i);
    if (limitComponents) fitVertex = theMerger->merge(fitVertex);
    ascendingFittedVertices.push_back(fitVertex);
  }

  // Initial vertex for descending fit
  priorVertexPosition = tracks[0]->linearizedTrack()->linearizationPoint();
  priorVertexError = GlobalError(we*10000);
  fitVertex = CachingVertex(priorVertexPosition,priorVertexError,initialTracks,0);

  // vertices from descending fit
  vector<CachingVertex> descendingFittedVertices;
  descendingFittedVertices.reserve(numberOfTracks);
  descendingFittedVertices.push_back(fitVertex);

  // descending fit
  for (vector<RefCountedVertexTrack>::const_iterator i = (tracks.end()-1);
	  i != (tracks.begin()); --i) {
    fitVertex = theUpdator.add(fitVertex,*i);
    if (limitComponents) fitVertex = theMerger->merge(fitVertex);
    descendingFittedVertices.insert(descendingFittedVertices.begin(), fitVertex);
  }

  vector<RefCountedVertexTrack> newTracks;
  double smoothedChi2 = 0.;  // Smoothed chi2

  // Track refit
  for(vector<RefCountedVertexTrack>::const_iterator i = tracks.begin();
  	i != tracks.end();i++)
  {
    int indexNumber = i-tracks.begin();
    //First, combine the vertices:
    VertexState meanedVertex = 
         meanVertex(ascendingFittedVertices[indexNumber].vertexState(), 
    		    descendingFittedVertices[indexNumber].vertexState());
    if (limitComponents) meanedVertex = theMerger->merge(meanedVertex);
    // Add the vertex and smooth the track:
    TrackChi2Pair thePair = vertexAndTrackUpdate(meanedVertex, *i, vertex.position());
    smoothedChi2 += thePair.second;
    newTracks.push_back( theVTFactory.vertexTrack((**i).linearizedTrack(),
  	vertex.vertexState(), thePair.first, AlgebraicMatrix(), 
	(**i).weight()) );
  }

  if  (vertex.hasPrior()) {
    smoothedChi2 += priorVertexChi2(vertex.priorVertexState(), vertex.vertexState());
    return CachingVertex(vertex.priorVertexState(), vertex.vertexState(),
    		newTracks, smoothedChi2);
  } else {
    return CachingVertex(vertex.vertexState(), newTracks, smoothedChi2);
  }
}

GsfVertexSmoother::TrackChi2Pair 
GsfVertexSmoother::vertexAndTrackUpdate(const VertexState & oldVertex,
	const RefCountedVertexTrack track, const GlobalPoint & referencePosition) const
{

  VSC prevVtxComponents = oldVertex.components();

  if (prevVtxComponents.empty()) {
  throw VertexException
    ("GsfVertexSmoother::(Previous) Vertex to update has no components");
  }

  LTC ltComponents = track->linearizedTrack()->components();
  if (ltComponents.empty()) {
  throw VertexException
    ("GsfVertexSmoother::Track to add to vertex has no components");
  }
  float trackWeight = track->weight();

  vector<RefittedTrackComponent> newTrackComponents;
  newTrackComponents.reserve(prevVtxComponents.size()*ltComponents.size());

  for (VSC::iterator vertexCompIter = prevVtxComponents.begin();
  	vertexCompIter != prevVtxComponents.end(); vertexCompIter++ ) {

    for (LTC::iterator trackCompIter = ltComponents.begin();
  	trackCompIter != ltComponents.end(); trackCompIter++ ) {
      newTrackComponents.push_back
        (createNewComponent(*vertexCompIter, *trackCompIter, trackWeight));
    }
  }

  return assembleTrackComponents(newTrackComponents, referencePosition);
}


GsfVertexSmoother::TrackChi2Pair GsfVertexSmoother::assembleTrackComponents(
	const vector<GsfVertexSmoother::RefittedTrackComponent> & trackComponents,
	const GlobalPoint & referencePosition)
	const
{

  //renormalize weights

  double totalWeight = 0.;
  double totalChi2 = 0.;

  for (vector<RefittedTrackComponent>::const_iterator iter = trackComponents.begin();
    iter != trackComponents.end(); ++iter ) {
    totalWeight += iter->second.first;
    totalChi2 += iter->second.second * iter->second.first ;
  }

  totalChi2 /= totalWeight;

  vector<RefCountedRefittedTrackState> reWeightedRTSC;
  reWeightedRTSC.reserve(trackComponents.size());
  

  for (vector<RefittedTrackComponent>::const_iterator iter = trackComponents.begin();
    iter != trackComponents.end(); ++iter ) {
    if (iter->second.first!=0) {
      reWeightedRTSC.push_back(iter->first->stateWithNewWeight(iter->second.first/totalWeight));
    }
  }

  RefCountedRefittedTrackState finalRTS = 
    RefCountedRefittedTrackState(new MultiRefittedTS(reWeightedRTSC, referencePosition));
  return TrackChi2Pair(finalRTS, totalChi2);
}


GsfVertexSmoother::RefittedTrackComponent 
GsfVertexSmoother::createNewComponent(const VertexState & oldVertex,
	 const RefCountedLinearizedTrackState linTrack, float weight) const
{

  int sign =+1;

  // Weight of the component in the mixture (non-normalized)
  double weightInMixture = theWeightCalculator.calculate(oldVertex, linTrack, 1000000000.);

  // position estimate of the component
  VertexState newVertex = kalmanVertexUpdator.positionUpdate(oldVertex,
	linTrack, weight, sign);

  pair<RefCountedRefittedTrackState, AlgebraicMatrix> thePair = 
  	theVertexTrackUpdator.trackRefit(newVertex, linTrack);

  //Chi**2 contribution of the track component
  float chi2 = smoothedChi2Estimator.trackParameterChi2(linTrack, thePair.first);

  return RefittedTrackComponent(thePair.first, 
  			WeightChi2Pair(weightInMixture, chi2));
}


VertexState
GsfVertexSmoother::meanVertex(const VertexState & vertexA,
			      const VertexState & vertexB) const
{
  vector<VertexState> vsCompA = vertexA.components();
  vector<VertexState> vsCompB = vertexB.components();
  vector<VertexState> finalVS;
  finalVS.reserve(vsCompA.size()*vsCompB.size());
  for (vector<VertexState>::iterator iA = vsCompA.begin(); iA!= vsCompA.end(); ++iA)
  {
    for (vector<VertexState>::iterator iB = vsCompB.begin(); iB!= vsCompB.end(); ++iB)
    {
      AlgebraicSymMatrix newWeight = iA->weight().matrix() +
				     iB->weight().matrix();
      AlgebraicVector newWtP = iA->weightTimesPosition() +
      			       iB->weightTimesPosition();
      double newWeightInMixture = iA->weightInMixture() *
				  iB->weightInMixture();
      finalVS.push_back( VertexState(newWtP, newWeight, newWeightInMixture) );
    }
  }
  #ifndef CMS_NO_COMPLEX_RETURNS
    return VertexState(new BasicMultiVertexState(finalVS));
  #else
    VertexState theFinalVM(new BasicMultiVertexState(finalVS));
    return theFinalVM;
  #endif
}


double GsfVertexSmoother::priorVertexChi2(
	const VertexState priorVertex, const VertexState fittedVertex) const
{
  vector<VertexState> priorVertexComp  = priorVertex.components();
  vector<VertexState> fittedVertexComp = fittedVertex.components();
  double vetexChi2 = 0.;
  for (vector<VertexState>::iterator pvI = priorVertexComp.begin(); 
  	pvI!= priorVertexComp.end(); ++pvI)
  {
    for (vector<VertexState>::iterator fvI = fittedVertexComp.begin(); 
    	fvI!= fittedVertexComp.end(); ++fvI)
    {
      vetexChi2 += (pvI->weightInMixture())*(fvI->weightInMixture())*
      			smoothedChi2Estimator.priorVertexChi2(*pvI, *fvI);
    }
  }
  return vetexChi2;
}
