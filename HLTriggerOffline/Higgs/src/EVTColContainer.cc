#ifndef HLTRIGGEROFFLINE_HIGGS_EVTCOLCONTAINER
#define HLTRIGGEROFFLINE_HIGGS_EVTCOLCONTAINER

/** \class EVTColContainer
*  Generate histograms for trigger efficiencies Higgs related
*  Documentation available on the CMS TWiki:
*  https://twiki.cern.ch/twiki/bin/view/CMS/HiggsWGHLTValidate
*
*  \author  J. Duarte Campderros
*
*/

#include "DataFormats/Common/interface/TriggerResults.h"
#include "DataFormats/HLTReco/interface/TriggerEventWithRefs.h" // TO BE DEPRECATED
#include "DataFormats/MuonReco/interface/Muon.h"
#include "DataFormats/MuonReco/interface/MuonFwd.h"
#include "DataFormats/EgammaCandidates/interface/GsfElectron.h"
#include "DataFormats/EgammaCandidates/interface/GsfElectronFwd.h"
#include "DataFormats/METReco/interface/CaloMET.h"
#include "DataFormats/METReco/interface/CaloMETFwd.h"
#include "DataFormats/METReco/interface/PFMET.h"
#include "DataFormats/METReco/interface/PFMETFwd.h"
#include "DataFormats/EgammaCandidates/interface/Photon.h"
#include "DataFormats/EgammaCandidates/interface/PhotonFwd.h"
#include "DataFormats/TrackReco/interface/Track.h"
#include "DataFormats/TrackReco/interface/TrackFwd.h"
#include "DataFormats/TauReco/interface/PFTau.h"
#include "DataFormats/TauReco/interface/PFTauFwd.h"
#include "DataFormats/HepMCCandidate/interface/GenParticle.h"
#include "DataFormats/JetReco/interface/GenJet.h"
#include "DataFormats/JetReco/interface/GenJetCollection.h"
#include "DataFormats/JetReco/interface/PFJet.h"
#include "DataFormats/JetReco/interface/PFJetCollection.h"
#include "DataFormats/BTauReco/interface/JetTag.h"

#include<vector>
#include<map>

//! container with all the objects needed
struct EVTColContainer
{
    enum 
    {
        MUON,
        ELEC,
        PHOTON,
        CALOMET,
        PFMET,
        PFTAU,
        PFJET,
//		TRACK,
        _nMAX
    };
    
    int nOfCollections;
    int nInitialized;
    const reco::GenParticleCollection * genParticles;
    const reco::GenJetCollection * genJets;
    const std::vector<reco::Muon> * muons;
    const std::vector<reco::GsfElectron> * electrons;
    const std::vector<reco::Photon> * photons;
    const std::vector<reco::CaloMET> * caloMETs;
    const std::vector<reco::PFMET> * pfMETs;
    const std::vector<reco::PFTau> * pfTaus;
    const std::vector<reco::PFJet> * pfJets;
    const reco::JetTagCollection * jetTags;
    //const std::vector<reco::Track> * tracks;
    const trigger::TriggerEventWithRefs * rawTriggerEvent;
    const edm::TriggerResults   * triggerResults ;
    EVTColContainer():
        nOfCollections(7),
        nInitialized(0),
        genParticles(0),
        genJets(0),
        muons(0),
        electrons(0),
        photons(0),
        caloMETs(0),
        pfMETs(0),
        pfTaus(0),
        pfJets(0),
        jetTags(0),
        //tracks(0),
        rawTriggerEvent(0),
        triggerResults(0)
    {
    }
    //! 
    bool isAllInit()
    {
        return (nInitialized == nOfCollections);
    }

    bool isCommonInit()
    {
        return (rawTriggerEvent != 0);
    }
    //! 
    void reset()
    {
        nInitialized = 0;
        genParticles = 0;
        genJets = 0;
        muons = 0; electrons = 0; photons = 0; pfTaus=0; caloMETs=0; pfMETs= 0; pfJets=0; //tracks=0; 
        jetTags = 0;
        rawTriggerEvent = 0;
        triggerResults = 0;
    }
    //! Setter: multiple overloaded function
    void set(const reco::MuonCollection * v)
    {
        muons = v;
        ++nInitialized;
    }
    void set(const reco::GsfElectronCollection * v)
    {
        electrons = v;
        ++nInitialized;
    }
    void set(const reco::PhotonCollection * v)
    {
        photons = v;
        ++nInitialized;
    }
    void set(const reco::CaloMETCollection * v)
    {
        caloMETs = v;
        ++nInitialized;
    }
    void set(const reco::PFMETCollection * v)
    {
        pfMETs = v;
        ++nInitialized;
    }
    void set(const reco::PFTauCollection * v)
    {
        pfTaus = v;
        ++nInitialized;
    }
    void set(const reco::PFJetCollection * v)
    {
        pfJets = v;
        ++nInitialized;
    }
    void set(const reco::JetTagCollection * v)
    {
        jetTags = v;
    }
    /*void set(const reco::TrackCollection * v)
    {
        tracks = v;
        ++nInitialized;
    }*/
    const unsigned int getSize(const unsigned int & objtype) const
    {
        unsigned int size = 0;
        if( objtype == EVTColContainer::MUON && muons != 0 )
        {
            size = muons->size();
        }
        else if( objtype == EVTColContainer::ELEC && electrons != 0 )
        {
            size = electrons->size();
        }
        else if( objtype == EVTColContainer::PHOTON && photons != 0 )
        {
            size = photons->size();
        }
        else if( objtype == EVTColContainer::CALOMET && caloMETs != 0 )
        {
            size = caloMETs->size();
        }
        else if( objtype == EVTColContainer::PFMET && pfMETs != 0 )
        {
            size = pfMETs->size();
        }
        else if( objtype == EVTColContainer::PFTAU && pfTaus != 0 )
        {
            size = pfTaus->size();
        }
        else if( objtype == EVTColContainer::PFJET && pfJets != 0 )
        {
            size = pfJets->size();
        }
        /*else if( objtype == EVTColContainer::TRACK && tracks != 0 )
        {
            size = tracks->size();
        }*/

        return size;
    }
    
    static std::string getTypeString(const unsigned int & objtype) 
    {
        std::string objTypestr;
        
        if( objtype == EVTColContainer::MUON )
        {
            objTypestr = "Mu";
        }
        else if( objtype == EVTColContainer::ELEC )
        {
            objTypestr = "Ele";
        }
        else if( objtype == EVTColContainer::PHOTON )
        {
            objTypestr = "Photon";
        }
        else if( objtype == EVTColContainer::CALOMET )
        {
            objTypestr = "MET";
        }
        else if( objtype == EVTColContainer::PFMET )
        {
            objTypestr = "PFMET";
        }
        else if( objtype == EVTColContainer::PFTAU )
        {
            objTypestr = "PFTau";
        }
        else if( objtype == EVTColContainer::PFJET )
        {
            objTypestr = "Jet";
        }
        /*else if( objtype == EVTColContainer::TRACK )
        {
            // FIXME: decide what to do! Just a patch
            objTypestr = "TkMu";
        }*/
        else
        { 
            edm::LogError("HiggsValidations") << "EVTColContainer::getTypeString, "
                << "NOT Implemented error (object type id='" << objtype << "')" << std::endl;;
        }
        
        return objTypestr;
    }
};
#endif
