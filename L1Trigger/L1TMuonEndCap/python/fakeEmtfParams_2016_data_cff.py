import FWCore.ParameterSet.Config as cms

## Fills CondFormats from the database
from CondCore.CondDB.CondDB_cfi import CondDB
CondDB.connect = cms.string("frontier://FrontierProd/CMS_CONDITIONS")


## Fills firmware, pT LUT, and PC LUT versions from the database
emtfParamsSource = cms.ESSource(
    "PoolDBESSource",
    CondDB,
    toGet   = cms.VPSet(
        cms.PSet(
            record = cms.string("L1TMuonEndcapParamsRcd"),
            tag    = cms.string("L1TMuonEndCapParams_Stage2v1_hlt")
            )
        )
    )


## Fills pT LUT XMLs ("forests") from the database
emtfForestsSource = cms.ESSource(
    "EmptyESSource",
    recordName = cms.string('L1TMuonEndCapForestRcd'),
    iovIsRunNotTime = cms.bool(True),
    firstValid = cms.vuint32(1)
    )

emtfForestsDB = cms.ESSource(
    "PoolDBESSource",
    CondDB,
    toGet   = cms.VPSet(
        cms.PSet(
            ## https://cms-conddb.cern.ch/cmsDbBrowser/search/Prod/L1TMuonEndCapForest
            record = cms.string("L1TMuonEndCapForestRcd"),
            ## v5 EMTF pT LUTs from ~August 2016
            tag = cms.string("L1TMuonEndCapForest_static_2016_mc")
            )
        )
    )

