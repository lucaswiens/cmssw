#include "FWCore/Framework/interface/EventProcessor.h"

#include "DataFormats/Provenance/interface/BranchIDListHelper.h"
#include "DataFormats/Provenance/interface/ModuleDescription.h"
#include "DataFormats/Provenance/interface/ParameterSetID.h"
#include "DataFormats/Provenance/interface/ParentageRegistry.h"
#include "DataFormats/Provenance/interface/ProcessHistoryRegistry.h"
#include "DataFormats/Provenance/interface/SubProcessParentageHelper.h"

#include "FWCore/Framework/interface/CommonParams.h"
#include "FWCore/Framework/interface/EDLooperBase.h"
#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/EventSetupProvider.h"
#include "FWCore/Framework/interface/EventSetupRecord.h"
#include "FWCore/Framework/interface/FileBlock.h"
#include "FWCore/Framework/interface/HistoryAppender.h"
#include "FWCore/Framework/interface/InputSourceDescription.h"
#include "FWCore/Framework/interface/IOVSyncValue.h"
#include "FWCore/Framework/interface/LooperFactory.h"
#include "FWCore/Framework/interface/LuminosityBlock.h"
#include "FWCore/Framework/interface/LuminosityBlockPrincipal.h"
#include "FWCore/Framework/interface/MessageReceiverForSource.h"
#include "FWCore/Framework/interface/ModuleChanger.h"
#include "FWCore/Framework/interface/OccurrenceTraits.h"
#include "FWCore/Framework/interface/ProcessingController.h"
#include "FWCore/Framework/interface/RunPrincipal.h"
#include "FWCore/Framework/interface/Schedule.h"
#include "FWCore/Framework/interface/ScheduleInfo.h"
#include "FWCore/Framework/interface/SubProcess.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/src/Breakpoints.h"
#include "FWCore/Framework/src/EPStates.h"
#include "FWCore/Framework/src/EventSetupsController.h"
#include "FWCore/Framework/src/InputSourceFactory.h"
#include "FWCore/Framework/src/SharedResourcesRegistry.h"
#include "FWCore/Framework/src/streamTransitionAsync.h"
#include "FWCore/Framework/src/globalTransitionAsync.h"

#include "FWCore/MessageLogger/interface/MessageLogger.h"

#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/IllegalParameters.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescriptionFillerBase.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescriptionFillerPluginFactory.h"
#include "FWCore/ParameterSet/interface/ProcessDesc.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/PythonParameterSet/interface/PythonProcessDesc.h"

#include "FWCore/ServiceRegistry/interface/ServiceRegistry.h"
#include "FWCore/ServiceRegistry/interface/Service.h"
#include "FWCore/ServiceRegistry/interface/StreamContext.h"
#include "FWCore/ServiceRegistry/interface/SystemBounds.h"

#include "FWCore/Concurrency/interface/WaitingTaskHolder.h"

#include "FWCore/Utilities/interface/Algorithms.h"
#include "FWCore/Utilities/interface/DebugMacros.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/ConvertException.h"
#include "FWCore/Utilities/interface/RandomNumberGenerator.h"
#include "FWCore/Utilities/interface/UnixSignalHandlers.h"
#include "FWCore/Utilities/interface/ExceptionCollector.h"
#include "FWCore/Utilities/interface/StreamID.h"
#include "FWCore/Utilities/interface/RootHandlers.h"
#include "FWCore/Utilities/interface/propagate_const.h"

#include "MessageForSource.h"
#include "MessageForParent.h"

#include "boost/thread/xtime.hpp"
#include "boost/range/adaptor/reversed.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <utility>
#include <sstream>

#include <sys/ipc.h>
#include <sys/msg.h>

#include "tbb/task.h"

//Used for forking
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/fcntl.h>
#include <unistd.h>


//Used for CPU affinity
#ifndef __APPLE__
#include <sched.h>
#endif

namespace {
  //Sentry class to only send a signal if an
  // exception occurs. An exception is identified
  // by the destructor being called without first
  // calling completedSuccessfully().
  class SendSourceTerminationSignalIfException {
  public:
    SendSourceTerminationSignalIfException(edm::ActivityRegistry* iReg):
      reg_(iReg) {}
    ~SendSourceTerminationSignalIfException() {
      if(reg_) {
        reg_->preSourceEarlyTerminationSignal_(edm::TerminationOrigin::ExceptionFromThisContext);
      }
    }
    void completedSuccessfully() {
      reg_ = nullptr;
    }
  private:
    edm::ActivityRegistry* reg_; // We do not use propagate_const because the registry itself is mutable.
  };

}

namespace edm {

  // ---------------------------------------------------------------
  std::unique_ptr<InputSource>
  makeInput(ParameterSet& params,
            CommonParams const& common,
            std::shared_ptr<ProductRegistry> preg,
            std::shared_ptr<BranchIDListHelper> branchIDListHelper,
            std::shared_ptr<ThinnedAssociationsHelper> thinnedAssociationsHelper,
            std::shared_ptr<ActivityRegistry> areg,
            std::shared_ptr<ProcessConfiguration const> processConfiguration,
            PreallocationConfiguration const& allocations) {
    ParameterSet* main_input = params.getPSetForUpdate("@main_input");
    if(main_input == 0) {
      throw Exception(errors::Configuration)
        << "There must be exactly one source in the configuration.\n"
        << "It is missing (or there are sufficient syntax errors such that it is not recognized as the source)\n";
    }

    std::string modtype(main_input->getParameter<std::string>("@module_type"));

    std::unique_ptr<ParameterSetDescriptionFillerBase> filler(
                                                              ParameterSetDescriptionFillerPluginFactory::get()->create(modtype));
    ConfigurationDescriptions descriptions(filler->baseType());
    filler->fill(descriptions);

    try {
      convertException::wrap([&]() {
          descriptions.validate(*main_input, std::string("source"));
        });
    }
    catch (cms::Exception & iException) {
      std::ostringstream ost;
      ost << "Validating configuration of input source of type " << modtype;
      iException.addContext(ost.str());
      throw;
    }

    main_input->registerIt();

    // Fill in "ModuleDescription", in case the input source produces
    // any EDProducts, which would be registered in the ProductRegistry.
    // Also fill in the process history item for this process.
    // There is no module label for the unnamed input source, so
    // just use "source".
    // Only the tracked parameters belong in the process configuration.
    ModuleDescription md(main_input->id(),
                         main_input->getParameter<std::string>("@module_type"),
                         "source",
                         processConfiguration.get(),
                         ModuleDescription::getUniqueID());

    InputSourceDescription isdesc(md, preg, branchIDListHelper, thinnedAssociationsHelper, areg,
                                  common.maxEventsInput_, common.maxLumisInput_,
                                  common.maxSecondsUntilRampdown_, allocations);

    areg->preSourceConstructionSignal_(md);
    std::unique_ptr<InputSource> input;
    try {
      //even if we have an exception, send the signal
      std::shared_ptr<int> sentry(nullptr,[areg,&md](void*){areg->postSourceConstructionSignal_(md);});
      convertException::wrap([&]() {
          input = std::unique_ptr<InputSource>(InputSourceFactory::get()->makeInputSource(*main_input, isdesc).release());
          input->preEventReadFromSourceSignal_.connect(std::cref(areg->preEventReadFromSourceSignal_));
          input->postEventReadFromSourceSignal_.connect(std::cref(areg->postEventReadFromSourceSignal_));
        });
    }
    catch (cms::Exception& iException) {
      std::ostringstream ost;
      ost << "Constructing input source of type " << modtype;
      iException.addContext(ost.str());
      throw;
    }
    return input;
  }

  // ---------------------------------------------------------------
  std::shared_ptr<EDLooperBase>
  fillLooper(eventsetup::EventSetupsController& esController,
             eventsetup::EventSetupProvider& cp,
             ParameterSet& params) {
    std::shared_ptr<EDLooperBase> vLooper;

    std::vector<std::string> loopers = params.getParameter<std::vector<std::string> >("@all_loopers");

    if(loopers.size() == 0) {
      return vLooper;
    }

    assert(1 == loopers.size());

    for(std::vector<std::string>::iterator itName = loopers.begin(), itNameEnd = loopers.end();
        itName != itNameEnd;
        ++itName) {

      ParameterSet* providerPSet = params.getPSetForUpdate(*itName);
      providerPSet->registerIt();
      vLooper = eventsetup::LooperFactory::get()->addTo(esController,
                                                        cp,
                                                        *providerPSet);
    }
    return vLooper;
  }

  // ---------------------------------------------------------------
  EventProcessor::EventProcessor(std::string const& config,
                                 ServiceToken const& iToken,
                                 serviceregistry::ServiceLegacy iLegacy,
                                 std::vector<std::string> const& defaultServices,
                                 std::vector<std::string> const& forcedServices) :
    actReg_(),
    preg_(),
    branchIDListHelper_(),
    serviceToken_(),
    input_(),
    espController_(new eventsetup::EventSetupsController),
    esp_(),
    act_table_(),
    processConfiguration_(),
    schedule_(),
    subProcesses_(),
    historyAppender_(new HistoryAppender),
    fb_(),
    looper_(),
    deferredExceptionPtrIsSet_(false),
    sourceResourcesAcquirer_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().first),
    sourceMutex_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().second),
    principalCache_(),
    beginJobCalled_(false),
    shouldWeStop_(false),
    stateMachineWasInErrorState_(false),
    fileMode_(),
    emptyRunLumiMode_(),
    exceptionMessageFiles_(),
    exceptionMessageRuns_(),
    exceptionMessageLumis_(),
    alreadyHandlingException_(false),
    forceLooperToEnd_(false),
    looperBeginJobRun_(false),
    forceESCacheClearOnNewRun_(false),
    numberOfForkedChildren_(0),
    numberOfSequentialEventsPerChild_(1),
    setCpuAffinity_(false),
    eventSetupDataToExcludeFromPrefetching_() {
    std::shared_ptr<ParameterSet> parameterSet = PythonProcessDesc(config).parameterSet();
    auto processDesc = std::make_shared<ProcessDesc>(parameterSet);
    processDesc->addServices(defaultServices, forcedServices);
    init(processDesc, iToken, iLegacy);
  }

  EventProcessor::EventProcessor(std::string const& config,
                                 std::vector<std::string> const& defaultServices,
                                 std::vector<std::string> const& forcedServices) :
    actReg_(),
    preg_(),
    branchIDListHelper_(),
    serviceToken_(),
    input_(),
    espController_(new eventsetup::EventSetupsController),
    esp_(),
    act_table_(),
    processConfiguration_(),
    schedule_(),
    subProcesses_(),
    historyAppender_(new HistoryAppender),
    fb_(),
    looper_(),
    deferredExceptionPtrIsSet_(false),
    sourceResourcesAcquirer_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().first),
    sourceMutex_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().second),
    principalCache_(),
    beginJobCalled_(false),
    shouldWeStop_(false),
    stateMachineWasInErrorState_(false),
    fileMode_(),
    emptyRunLumiMode_(),
    exceptionMessageFiles_(),
    exceptionMessageRuns_(),
    exceptionMessageLumis_(),
    alreadyHandlingException_(false),
    forceLooperToEnd_(false),
    looperBeginJobRun_(false),
    forceESCacheClearOnNewRun_(false),
    numberOfForkedChildren_(0),
    numberOfSequentialEventsPerChild_(1),
    setCpuAffinity_(false),
    asyncStopRequestedWhileProcessingEvents_(false),
    nextItemTypeFromProcessingEvents_(InputSource::IsEvent),
    eventSetupDataToExcludeFromPrefetching_()
  {
    std::shared_ptr<ParameterSet> parameterSet = PythonProcessDesc(config).parameterSet();
    auto processDesc = std::make_shared<ProcessDesc>(parameterSet);
    processDesc->addServices(defaultServices, forcedServices);
    init(processDesc, ServiceToken(), serviceregistry::kOverlapIsError);
  }

  EventProcessor::EventProcessor(std::shared_ptr<ProcessDesc> processDesc,
                                 ServiceToken const& token,
                                 serviceregistry::ServiceLegacy legacy) :
    actReg_(),
    preg_(),
    branchIDListHelper_(),
    serviceToken_(),
    input_(),
    espController_(new eventsetup::EventSetupsController),
    esp_(),
    act_table_(),
    processConfiguration_(),
    schedule_(),
    subProcesses_(),
    historyAppender_(new HistoryAppender),
    fb_(),
    looper_(),
    deferredExceptionPtrIsSet_(false),
    sourceResourcesAcquirer_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().first),
    sourceMutex_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().second),
    principalCache_(),
    beginJobCalled_(false),
    shouldWeStop_(false),
    stateMachineWasInErrorState_(false),
    fileMode_(),
    emptyRunLumiMode_(),
    exceptionMessageFiles_(),
    exceptionMessageRuns_(),
    exceptionMessageLumis_(),
    alreadyHandlingException_(false),
    forceLooperToEnd_(false),
    looperBeginJobRun_(false),
    forceESCacheClearOnNewRun_(false),
    numberOfForkedChildren_(0),
    numberOfSequentialEventsPerChild_(1),
    setCpuAffinity_(false),
    asyncStopRequestedWhileProcessingEvents_(false),
    nextItemTypeFromProcessingEvents_(InputSource::IsEvent),
    eventSetupDataToExcludeFromPrefetching_()
  {
    init(processDesc, token, legacy);
  }


  EventProcessor::EventProcessor(std::string const& config, bool isPython):
    actReg_(),
    preg_(),
    branchIDListHelper_(),
    serviceToken_(),
    input_(),
    espController_(new eventsetup::EventSetupsController),
    esp_(),
    act_table_(),
    processConfiguration_(),
    schedule_(),
    subProcesses_(),
    historyAppender_(new HistoryAppender),
    fb_(),
    looper_(),
    deferredExceptionPtrIsSet_(false),
    sourceResourcesAcquirer_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().first),
    sourceMutex_(SharedResourcesRegistry::instance()->createAcquirerForSourceDelayedReader().second),
    principalCache_(),
    beginJobCalled_(false),
    shouldWeStop_(false),
    stateMachineWasInErrorState_(false),
    fileMode_(),
    emptyRunLumiMode_(),
    exceptionMessageFiles_(),
    exceptionMessageRuns_(),
    exceptionMessageLumis_(),
    alreadyHandlingException_(false),
    forceLooperToEnd_(false),
    looperBeginJobRun_(false),
    forceESCacheClearOnNewRun_(false),
    numberOfForkedChildren_(0),
    numberOfSequentialEventsPerChild_(1),
    setCpuAffinity_(false),
    asyncStopRequestedWhileProcessingEvents_(false),
    nextItemTypeFromProcessingEvents_(InputSource::IsEvent),
    eventSetupDataToExcludeFromPrefetching_()
  {
    if(isPython) {
      std::shared_ptr<ParameterSet> parameterSet = PythonProcessDesc(config).parameterSet();
      auto processDesc = std::make_shared<ProcessDesc>(parameterSet);
      init(processDesc, ServiceToken(), serviceregistry::kOverlapIsError);
    }
    else {
      auto processDesc = std::make_shared<ProcessDesc>(config);
      init(processDesc, ServiceToken(), serviceregistry::kOverlapIsError);
    }
  }

  void
  EventProcessor::init(std::shared_ptr<ProcessDesc>& processDesc,
                       ServiceToken const& iToken,
                       serviceregistry::ServiceLegacy iLegacy) {

    //std::cerr << processDesc->dump() << std::endl;

    // register the empty parentage vector , once and for all
    ParentageRegistry::instance()->insertMapped(Parentage());

    // register the empty parameter set, once and for all.
    ParameterSet().registerIt();

    std::shared_ptr<ParameterSet> parameterSet = processDesc->getProcessPSet();

    // If there are subprocesses, pop the subprocess parameter sets out of the process parameter set
    auto subProcessVParameterSet = popSubProcessVParameterSet(*parameterSet);
    bool const hasSubProcesses = !subProcessVParameterSet.empty();

    // Now set some parameters specific to the main process.
    ParameterSet const& optionsPset(parameterSet->getUntrackedParameterSet("options", ParameterSet()));
    fileMode_ = optionsPset.getUntrackedParameter<std::string>("fileMode", "");
    emptyRunLumiMode_ = optionsPset.getUntrackedParameter<std::string>("emptyRunLumiMode", "");
    forceESCacheClearOnNewRun_ = optionsPset.getUntrackedParameter<bool>("forceEventSetupCacheClearOnNewRun", false);
    //threading
    unsigned int nThreads=1;
    if(optionsPset.existsAs<unsigned int>("numberOfThreads",false)) {
      nThreads = optionsPset.getUntrackedParameter<unsigned int>("numberOfThreads");
      if(nThreads == 0) {
        nThreads = 1;
      }
    }
    /* TODO: when we support having each stream run in a different thread use this default
       unsigned int nStreams =nThreads;
    */
    unsigned int nStreams =1;
    if(optionsPset.existsAs<unsigned int>("numberOfStreams",false)) {
      nStreams = optionsPset.getUntrackedParameter<unsigned int>("numberOfStreams");
      if(nStreams==0) {
        nStreams = nThreads;
      }
    }
    if(nThreads >1) {
      edm::LogInfo("ThreadStreamSetup") <<"setting # threads "<<nThreads<<"\nsetting # streams "<<nStreams;
    }

    /*
      bool nRunsSet = false;
    */
    unsigned int nConcurrentRuns =1;
    /*
      if(nRunsSet = optionsPset.existsAs<unsigned int>("numberOfConcurrentRuns",false)) {
      nConcurrentRuns = optionsPset.getUntrackedParameter<unsigned int>("numberOfConcurrentRuns");
      }
    */
    unsigned int nConcurrentLumis =1;
    /*
      if(optionsPset.existsAs<unsigned int>("numberOfConcurrentLuminosityBlocks",false)) {
      nConcurrentLumis = optionsPset.getUntrackedParameter<unsigned int>("numberOfConcurrentLuminosityBlocks");
      } else {
      nConcurrentLumis = nConcurrentRuns;
      }
    */
    //Check that relationships between threading parameters makes sense
    /*
      if(nThreads<nStreams) {
      //bad
      }
      if(nConcurrentRuns>nStreams) {
      //bad
      }
      if(nConcurrentRuns>nConcurrentLumis) {
      //bad
      }
    */
    //forking
    ParameterSet const& forking = optionsPset.getUntrackedParameterSet("multiProcesses", ParameterSet());
    numberOfForkedChildren_ = forking.getUntrackedParameter<int>("maxChildProcesses", 0);
    numberOfSequentialEventsPerChild_ = forking.getUntrackedParameter<unsigned int>("maxSequentialEventsPerChild", 1);
    setCpuAffinity_ = forking.getUntrackedParameter<bool>("setCpuAffinity", false);
    continueAfterChildFailure_ = forking.getUntrackedParameter<bool>("continueAfterChildFailure",false);
    std::vector<ParameterSet> const& excluded = forking.getUntrackedParameterSetVector("eventSetupDataToExcludeFromPrefetching", std::vector<ParameterSet>());
    for(auto const& ps : excluded) {
      eventSetupDataToExcludeFromPrefetching_[ps.getUntrackedParameter<std::string>("record")].emplace(ps.getUntrackedParameter<std::string>("type", "*"),
                                                                                                       ps.getUntrackedParameter<std::string>("label", ""));
    }
    IllegalParameters::setThrowAnException(optionsPset.getUntrackedParameter<bool>("throwIfIllegalParameter", true));

    printDependencies_ =  optionsPset.getUntrackedParameter("printDependencies", false);

    // Now do general initialization
    ScheduleItems items;

    //initialize the services
    auto& serviceSets = processDesc->getServicesPSets();
    ServiceToken token = items.initServices(serviceSets, *parameterSet, iToken, iLegacy, true);
    serviceToken_ = items.addCPRandTNS(*parameterSet, token);

    //make the services available
    ServiceRegistry::Operate operate(serviceToken_);

    if(nStreams>1) {
      edm::Service<RootHandlers> handler;
      handler->willBeUsingThreads();
    }

    // intialize miscellaneous items
    std::shared_ptr<CommonParams> common(items.initMisc(*parameterSet));

    // intialize the event setup provider
    esp_ = espController_->makeProvider(*parameterSet);

    // initialize the looper, if any
    looper_ = fillLooper(*espController_, *esp_, *parameterSet);
    if(looper_) {
      looper_->setActionTable(items.act_table_.get());
      looper_->attachTo(*items.actReg_);

      //For now loopers make us run only 1 transition at a time
      nStreams=1;
      nConcurrentLumis=1;
      nConcurrentRuns=1;
    }

    preallocations_ = PreallocationConfiguration{nThreads,nStreams,nConcurrentLumis,nConcurrentRuns};

    // initialize the input source
    input_ = makeInput(*parameterSet,
                       *common,
                       items.preg(),
                       items.branchIDListHelper(),
                       items.thinnedAssociationsHelper(),
                       items.actReg_,
                       items.processConfiguration(),
                       preallocations_);

    // intialize the Schedule
    schedule_ = items.initSchedule(*parameterSet,hasSubProcesses,preallocations_,&processContext_);

    // set the data members
    act_table_ = std::move(items.act_table_);
    actReg_ = items.actReg_;
    preg_ = items.preg();
    branchIDListHelper_ = items.branchIDListHelper();
    thinnedAssociationsHelper_ = items.thinnedAssociationsHelper();
    processConfiguration_ = items.processConfiguration();
    processContext_.setProcessConfiguration(processConfiguration_.get());
    principalCache_.setProcessHistoryRegistry(input_->processHistoryRegistry());

    FDEBUG(2) << parameterSet << std::endl;

    principalCache_.setNumberOfConcurrentPrincipals(preallocations_);
    for(unsigned int index = 0; index<preallocations_.numberOfStreams(); ++index ) {
      // Reusable event principal
      auto ep = std::make_shared<EventPrincipal>(preg(), branchIDListHelper(),
                                                 thinnedAssociationsHelper(), *processConfiguration_, historyAppender_.get(), index);
      principalCache_.insert(ep);
    }

    // fill the subprocesses, if there are any
    subProcesses_.reserve(subProcessVParameterSet.size());
    for(auto& subProcessPSet : subProcessVParameterSet) {
      subProcesses_.emplace_back(subProcessPSet,
                                 *parameterSet,
                                 preg(),
                                 branchIDListHelper(),
                                 *thinnedAssociationsHelper_,
                                 SubProcessParentageHelper(),
                                 *espController_,
                                 *actReg_,
                                 token,
                                 serviceregistry::kConfigurationOverrides,
                                 preallocations_,
                                 &processContext_);
    }
  }

  EventProcessor::~EventProcessor() {
    // Make the services available while everything is being deleted.
    ServiceToken token = getToken();
    ServiceRegistry::Operate op(token);

    // manually destroy all these thing that may need the services around
    // propagate_const<T> has no reset() function
    espController_ = nullptr;
    esp_ = nullptr;
    schedule_ = nullptr;
    input_ = nullptr;
    looper_ = nullptr;
    actReg_ = nullptr;

    pset::Registry::instance()->clear();
    ParentageRegistry::instance()->clear();
  }

  void
  EventProcessor::beginJob() {
    if(beginJobCalled_) return;
    beginJobCalled_=true;
    bk::beginJob();

    // StateSentry toerror(this); // should we add this ?
    //make the services available
    ServiceRegistry::Operate operate(serviceToken_);

    service::SystemBounds bounds(preallocations_.numberOfStreams(),
                                 preallocations_.numberOfLuminosityBlocks(),
                                 preallocations_.numberOfRuns(),
                                 preallocations_.numberOfThreads());
    actReg_->preallocateSignal_(bounds);
    schedule_->convertCurrentProcessAlias(processConfiguration_->processName());
    pathsAndConsumesOfModules_.initialize(schedule_.get(), preg());

    //NOTE: this may throw
    checkForModuleDependencyCorrectness(pathsAndConsumesOfModules_, printDependencies_);
    actReg_->preBeginJobSignal_(pathsAndConsumesOfModules_, processContext_);

    //NOTE:  This implementation assumes 'Job' means one call
    // the EventProcessor::run
    // If it really means once per 'application' then this code will
    // have to be changed.
    // Also have to deal with case where have 'run' then new Module
    // added and do 'run'
    // again.  In that case the newly added Module needs its 'beginJob'
    // to be called.

    //NOTE: in future we should have a beginOfJob for looper that takes no arguments
    //  For now we delay calling beginOfJob until first beginOfRun
    //if(looper_) {
    //   looper_->beginOfJob(es);
    //}
    try {
      convertException::wrap([&]() {
          input_->doBeginJob();
        });
    }
    catch(cms::Exception& ex) {
      ex.addContext("Calling beginJob for the source");
      throw;
    }
    schedule_->beginJob(*preg_);
    // toerror.succeeded(); // should we add this?
    for_all(subProcesses_, [](auto& subProcess){ subProcess.doBeginJob(); });
    actReg_->postBeginJobSignal_();

    for(unsigned int i=0; i<preallocations_.numberOfStreams();++i) {
      schedule_->beginStream(i);
      for_all(subProcesses_, [i](auto& subProcess){ subProcess.doBeginStream(i); });
    }
  }

  void
  EventProcessor::endJob() {
    // Collects exceptions, so we don't throw before all operations are performed.
    ExceptionCollector c("Multiple exceptions were thrown while executing endJob. An exception message follows for each.\n");

    //make the services available
    ServiceRegistry::Operate operate(serviceToken_);

    //NOTE: this really should go elsewhere in the future
    for(unsigned int i=0; i<preallocations_.numberOfStreams();++i) {
      c.call([this,i](){this->schedule_->endStream(i);});
      for(auto& subProcess : subProcesses_) {
        c.call([&subProcess,i](){ subProcess.doEndStream(i); } );
      }
    }
    auto actReg = actReg_.get();
    c.call([actReg](){actReg->preEndJobSignal_();});
    schedule_->endJob(c);
    for(auto& subProcess : subProcesses_) {
      c.call(std::bind(&SubProcess::doEndJob, &subProcess));
    }
    c.call(std::bind(&InputSource::doEndJob, input_.get()));
    if(looper_) {
      c.call(std::bind(&EDLooperBase::endOfJob, looper()));
    }
    c.call([actReg](){actReg->postEndJobSignal_();});
    if(c.hasThrown()) {
      c.rethrow();
    }
  }

  ServiceToken
  EventProcessor::getToken() {
    return serviceToken_;
  }

  //Setup signal handler to listen for when forked children stop
  namespace {
    //These are volatile since the compiler can not be allowed to optimize them
    // since they can be modified in the signaller handler
    volatile bool child_failed = false;
    volatile unsigned int num_children_done = 0;
    volatile int child_fail_exit_status = 0;
    volatile int child_fail_signal = 0;

    //NOTE: We setup the signal handler to run in the main thread which
    // is also the same thread that then reads the above values

    extern "C" {
      void ep_sigchld(int, siginfo_t*, void*) {
        //printf("in sigchld\n");
        //FDEBUG(1) << "in sigchld handler\n";
        int stat_loc;
        pid_t p = waitpid(-1, &stat_loc, WNOHANG);
        while(0<p) {
          //printf("  looping\n");
          if(WIFEXITED(stat_loc)) {
            ++num_children_done;
            if(0 != WEXITSTATUS(stat_loc)) {
              child_fail_exit_status = WEXITSTATUS(stat_loc);
              child_failed = true;
            }
          }
          if(WIFSIGNALED(stat_loc)) {
            ++num_children_done;
            child_fail_signal = WTERMSIG(stat_loc);
            child_failed = true;
          }
          p = waitpid(-1, &stat_loc, WNOHANG);
        }
      }
    }

  }

  enum {
    kChildSucceed,
    kChildExitBadly,
    kChildSegv,
    kMaxChildAction
  };

  namespace {
    unsigned int numberOfDigitsInChildIndex(unsigned int numberOfChildren) {
      unsigned int n = 0;
      while(numberOfChildren != 0) {
        ++n;
        numberOfChildren /= 10;
      }
      if(n == 0) {
        n = 3; // Protect against zero numberOfChildren
      }
      return n;
    }

    /*This class embodied the thread which is used to listen to the forked children and
      then tell them which events they should process */
    class MessageSenderToSource {
    public:
      MessageSenderToSource(std::vector<int> const& childrenSockets, std::vector<int> const& childrenPipes, long iNEventsToProcess);
      void operator()();

    private:
      const std::vector<int>& m_childrenPipes;
      long const m_nEventsToProcess;
      fd_set m_socketSet;
      unsigned int m_aliveChildren;
      int m_maxFd;
    };

    MessageSenderToSource::MessageSenderToSource(std::vector<int> const& childrenSockets,
                                                 std::vector<int> const& childrenPipes,
                                                 long iNEventsToProcess):
      m_childrenPipes(childrenPipes),
      m_nEventsToProcess(iNEventsToProcess),
      m_aliveChildren(childrenSockets.size()),
      m_maxFd(0)
    {
      FD_ZERO(&m_socketSet);
      for (auto const socket : childrenSockets) {
        FD_SET(socket, &m_socketSet);
        if (socket > m_maxFd) {
          m_maxFd = socket;
        }
      }
      for (auto const pipe : childrenPipes) {
        FD_SET(pipe, &m_socketSet);
        if (pipe > m_maxFd) {
          m_maxFd = pipe;
        }
      }
      m_maxFd++; // select reads [0,m_maxFd).
    }

    /* This function is the heart of the communication between parent and child.
     * When ready for more data, the child (see MessageReceiverForSource) requests
     * data through a AF_UNIX socket message.  The parent will then assign the next
     * chunk of data by sending a message back.
     *
     * Additionally, this function also monitors the read-side of the pipe fd from the child.
     * If the child dies unexpectedly, the pipe will be selected as ready for read and
     * will return EPIPE when read from.  Further, if the child thinks the parent has died
     * (defined as waiting more than 1s for a response), it will write a single byte to
     * the pipe.  If the parent has died, the child will get a EPIPE and throw an exception.
     * If still alive, the parent will read the byte and ignore it.
     *
     * Note this function is complemented by the SIGCHLD handler above as currently only the SIGCHLD
     * handler can distinguish between success and failure cases.
     */

    void
    MessageSenderToSource::operator()() {
      multicore::MessageForParent childMsg;
      LogInfo("ForkingController") << "I am controller";
      //this is the master and therefore the controller

      multicore::MessageForSource sndmsg;
      sndmsg.startIndex = 0;
      sndmsg.nIndices = m_nEventsToProcess;
      do {

        fd_set readSockets, errorSockets;
        // Wait for a request from a child for events.
        memcpy(&readSockets, &m_socketSet, sizeof(m_socketSet));
        memcpy(&errorSockets, &m_socketSet, sizeof(m_socketSet));
        // Note that we don't timeout; may be reconsidered in the future.
        ssize_t rc;
        while (((rc = select(m_maxFd, &readSockets, NULL, &errorSockets, NULL)) < 0) && (errno == EINTR)) {}
        if (rc < 0) {
          std::cerr << "select failed; should be impossible due to preconditions.\n";
          abort();
          break;
        }

        // Read the message from the child.
        for (int idx=0; idx<m_maxFd; idx++) {

          // Handle errors
          if (FD_ISSET(idx, &errorSockets)) {
            LogInfo("ForkingController") << "Error on socket " << idx;
            FD_CLR(idx, &m_socketSet);
            close(idx);
            // See if it was the watchdog pipe that died.
            for (std::vector<int>::const_iterator it = m_childrenPipes.begin(); it != m_childrenPipes.end(); it++) {
              if (*it == idx) {
                m_aliveChildren--;
              }
            }
            continue;
          }

          if (!FD_ISSET(idx, &readSockets)) {
            continue;
          }

          // See if this FD is a child watchdog pipe.  If so, read from it to prevent
          // writes from blocking.
          bool is_pipe = false;
          for (std::vector<int>::const_iterator it = m_childrenPipes.begin(), itEnd = m_childrenPipes.end(); it != itEnd; it++) {
            if (*it == idx) {
              is_pipe = true;
              char buf;
              while (((rc = read(idx, &buf, 1)) < 0) && (errno == EINTR)) {}
              if (rc <= 0) {
                m_aliveChildren--;
                FD_CLR(idx, &m_socketSet);
                close(idx);
              }
            }
          }

          // Only execute this block if the FD is a socket for sending the child work.
          if (!is_pipe) {
            while (((rc = recv(idx, reinterpret_cast<char*>(&childMsg),childMsg.sizeForBuffer() , 0)) < 0) && (errno == EINTR)) {}
            if (rc < 0) {
              FD_CLR(idx, &m_socketSet);
              close(idx);
              continue;
            }

            // Tell the child what events to process.
            // If 'send' fails, then the child process has failed (any other possibilities are
            // eliminated because we are using fixed-size messages with Unix datagram sockets).
            // Thus, the SIGCHLD handler will fire and set child_fail = true.
            while (((rc = send(idx, (char *)(&sndmsg), multicore::MessageForSource::sizeForBuffer(), 0)) < 0) && (errno == EINTR)) {}
            if (rc < 0) {
              FD_CLR(idx, &m_socketSet);
              close(idx);
              continue;
            }
            //std::cout << "Sent chunk starting at " << sndmsg.startIndex << " to child, length " << sndmsg.nIndices << std::endl;
            sndmsg.startIndex += sndmsg.nIndices;
          }
        }

      } while (m_aliveChildren > 0);

      return;
    }

  }


  void EventProcessor::possiblyContinueAfterForkChildFailure() {
    if(child_failed && continueAfterChildFailure_) {
      if (child_fail_signal) {
        LogSystem("ForkedChildFailed") << "child process ended abnormally with signal " << child_fail_signal;
        child_fail_signal=0;
      } else if (child_fail_exit_status) {
        LogSystem("ForkedChildFailed") << "child process ended abnormally with exit code " << child_fail_exit_status;
        child_fail_exit_status=0;
      } else {
        LogSystem("ForkedChildFailed") << "child process ended abnormally for unknown reason";
      }
      child_failed =false;
    }
  }

  bool
  EventProcessor::forkProcess(std::string const& jobReportFile) {

    if(0 == numberOfForkedChildren_) {return true;}
    assert(0<numberOfForkedChildren_);
    //do what we want done in common
    {
      beginJob(); //make sure this was run
      // make the services available
      ServiceRegistry::Operate operate(serviceToken_);

      InputSource::ItemType itemType;
      itemType = input_->nextItemType();

      assert(itemType == InputSource::IsFile);
      {
        readFile();
      }
      itemType = input_->nextItemType();
      assert(itemType == InputSource::IsRun);

      LogSystem("ForkingEventSetupPreFetching") << " prefetching for run " << input_->runAuxiliary()->run();
      IOVSyncValue ts(EventID(input_->runAuxiliary()->run(), 0, 0),
                      input_->runAuxiliary()->beginTime());
      espController_->eventSetupForInstance(ts);
      EventSetup const& es = esp_->eventSetup();

      //now get all the data available in the EventSetup
      std::vector<eventsetup::EventSetupRecordKey> recordKeys;
      es.fillAvailableRecordKeys(recordKeys);
      std::vector<eventsetup::DataKey> dataKeys;
      for(std::vector<eventsetup::EventSetupRecordKey>::const_iterator itKey = recordKeys.begin(), itEnd = recordKeys.end();
          itKey != itEnd;
          ++itKey) {
        eventsetup::EventSetupRecord const* recordPtr = es.find(*itKey);
        //see if this is on our exclusion list
        ExcludedDataMap::const_iterator itExcludeRec = eventSetupDataToExcludeFromPrefetching_.find(itKey->type().name());
        ExcludedData const* excludedData(nullptr);
        if(itExcludeRec != eventSetupDataToExcludeFromPrefetching_.end()) {
          excludedData = &(itExcludeRec->second);
          if(excludedData->size() == 0 || excludedData->begin()->first == "*") {
            //skip all items in this record
            continue;
          }
        }
        if(0 != recordPtr) {
          dataKeys.clear();
          recordPtr->fillRegisteredDataKeys(dataKeys);
          for(std::vector<eventsetup::DataKey>::const_iterator itDataKey = dataKeys.begin(), itDataKeyEnd = dataKeys.end();
              itDataKey != itDataKeyEnd;
              ++itDataKey) {
            //std::cout << "  " << itDataKey->type().name() << " " << itDataKey->name().value() << std::endl;
            if(0 != excludedData && excludedData->find(std::make_pair(itDataKey->type().name(), itDataKey->name().value())) != excludedData->end()) {
              LogInfo("ForkingEventSetupPreFetching") << "   excluding:" << itDataKey->type().name() << " " << itDataKey->name().value() << std::endl;
              continue;
            }
            try {
              recordPtr->doGet(*itDataKey);
            } catch(cms::Exception& e) {
              LogWarning("ForkingEventSetupPreFetching") << e.what();
            }
          }
        }
      }
    }
    LogSystem("ForkingEventSetupPreFetching") <<"  done prefetching";
    {
      // make the services available
      ServiceRegistry::Operate operate(serviceToken_);
      Service<JobReport> jobReport;
      jobReport->parentBeforeFork(jobReportFile, numberOfForkedChildren_);

      //Now actually do the forking
      actReg_->preForkReleaseResourcesSignal_();
      input_->doPreForkReleaseResources();
      schedule_->preForkReleaseResources();
    }
    installCustomHandler(SIGCHLD, ep_sigchld);


    unsigned int childIndex = 0;
    unsigned int const kMaxChildren = numberOfForkedChildren_;
    unsigned int const numberOfDigitsInIndex = numberOfDigitsInChildIndex(kMaxChildren);
    std::vector<pid_t> childrenIds;
    childrenIds.reserve(kMaxChildren);
    std::vector<int> childrenSockets;
    childrenSockets.reserve(kMaxChildren);
    std::vector<int> childrenPipes;
    childrenPipes.reserve(kMaxChildren);
    std::vector<int> childrenSocketsCopy;
    childrenSocketsCopy.reserve(kMaxChildren);
    std::vector<int> childrenPipesCopy;
    childrenPipesCopy.reserve(kMaxChildren);
    int pipes[] {0, 0};

    {
      // make the services available
      ServiceRegistry::Operate operate(serviceToken_);
      Service<JobReport> jobReport;
      int sockets[2], fd_flags;
      for(; childIndex < kMaxChildren; ++childIndex) {
        // Create a UNIX_DGRAM socket pair
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets)) {
          printf("Error creating communication socket (errno=%d, %s)\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        if (pipe(pipes)) {
          printf("Error creating communication pipes (errno=%d, %s)\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        // set CLOEXEC so the socket/pipe doesn't get leaked if the child exec's.
        if ((fd_flags = fcntl(sockets[1], F_GETFD, NULL)) == -1) {
          printf("Failed to get fd flags: %d %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        // Mark socket as non-block.  Child must be careful to do select prior
        // to reading from socket.
        if (fcntl(sockets[1], F_SETFD, fd_flags | FD_CLOEXEC | O_NONBLOCK) == -1) {
          printf("Failed to set new fd flags: %d %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        if ((fd_flags = fcntl(pipes[1], F_GETFD, NULL)) == -1) {
          printf("Failed to get fd flags: %d %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        if (fcntl(pipes[1], F_SETFD, fd_flags | FD_CLOEXEC) == -1) {
          printf("Failed to set new fd flags: %d %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        // Linux man page notes there are some edge cases where reading from a
        // fd can block, even after a select.
        if ((fd_flags = fcntl(pipes[0], F_GETFD, NULL)) == -1) {
          printf("Failed to get fd flags: %d %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }
        if (fcntl(pipes[0], F_SETFD, fd_flags | O_NONBLOCK) == -1) {
          printf("Failed to set new fd flags: %d %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        }

        childrenPipesCopy = childrenPipes;
        childrenSocketsCopy = childrenSockets;

        pid_t value = fork();
        if(value == 0) {
          // Close the parent's side of the socket and pipe which will talk to us.
          close(pipes[0]);
          close(sockets[0]);
          // Close our copies of the parent's other communication pipes.
          for(std::vector<int>::const_iterator it=childrenPipesCopy.begin(); it != childrenPipesCopy.end(); it++) {
            close(*it);
          }
          for(std::vector<int>::const_iterator it=childrenSocketsCopy.begin(); it != childrenSocketsCopy.end(); it++) {
            close(*it);
          }

          // this is the child process, redirect stdout and stderr to a log file
          fflush(stdout);
          fflush(stderr);
          std::stringstream stout;
          stout << "redirectout_" << getpgrp() << "_" << std::setw(numberOfDigitsInIndex) << std::setfill('0') << childIndex << ".log";
          if(0 == freopen(stout.str().c_str(), "w", stdout)) {
            LogError("ForkingStdOutRedirect") << "Error during freopen of child process "<< childIndex;
          }
          if(dup2(fileno(stdout), fileno(stderr)) < 0) {
            LogError("ForkingStdOutRedirect") << "Error during dup2 of child process"<< childIndex;
          }

          LogInfo("ForkingChild") << "I am child " << childIndex << " with pgid " << getpgrp();
          if(setCpuAffinity_) {
            // CPU affinity is handled differently on macosx.
            // We disable it and print a message until someone reads:
            //
            // http://developer.apple.com/mac/library/releasenotes/Performance/RN-AffinityAPI/index.html
            //
            // and implements it.
#ifdef __APPLE__
            LogInfo("ForkingChildAffinity") << "Architecture support for CPU affinity not implemented.";
#else
            LogInfo("ForkingChildAffinity") << "Setting CPU affinity, setting this child to cpu " << childIndex;
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(childIndex, &mask);
            if(sched_setaffinity(0, sizeof(mask), &mask) != 0) {
              LogError("ForkingChildAffinity") << "Failed to set the cpu affinity, errno " << errno;
              exit(-1);
            }
#endif
          }
          break;
        } else {
          //this is the parent
          close(pipes[1]);
          close(sockets[1]);
        }
        if(value < 0) {
          LogError("ForkingChild") << "failed to create a child";
          exit(-1);
        }
        childrenIds.push_back(value);
        childrenSockets.push_back(sockets[0]);
        childrenPipes.push_back(pipes[0]);
      }

      if(childIndex < kMaxChildren) {
        jobReport->childAfterFork(jobReportFile, childIndex, kMaxChildren);
        actReg_->postForkReacquireResourcesSignal_(childIndex, kMaxChildren);

        auto receiver = std::make_shared<multicore::MessageReceiverForSource>(sockets[1], pipes[1]);
        input_->doPostForkReacquireResources(receiver);
        schedule_->postForkReacquireResources(childIndex, kMaxChildren);
        //NOTE: sources have to reset themselves by listening to the post fork message
        //rewindInput();
        return true;
      }
      jobReport->parentAfterFork(jobReportFile);
    }

    //this is the original, which is now the master for all the children

    //Need to wait for signals from the children or externally
    // To wait we must
    // 1) block the signals we want to wait on so we do not have a race condition
    // 2) check that we haven't already meet our ending criteria
    // 3) call sigsuspend, which unblocks the signals and waits until a signal is caught
    sigset_t blockingSigSet;
    sigset_t unblockingSigSet;
    sigset_t oldSigSet;
    pthread_sigmask(SIG_SETMASK, NULL, &unblockingSigSet);
    pthread_sigmask(SIG_SETMASK, NULL, &blockingSigSet);
    sigaddset(&blockingSigSet, SIGCHLD);
    sigaddset(&blockingSigSet, SIGUSR2);
    sigaddset(&blockingSigSet, SIGINT);
    sigdelset(&unblockingSigSet, SIGCHLD);
    sigdelset(&unblockingSigSet, SIGUSR2);
    sigdelset(&unblockingSigSet, SIGINT);
    pthread_sigmask(SIG_BLOCK, &blockingSigSet, &oldSigSet);

    // If there are too many fd's (unlikely, but possible) for select, denote this
    // because the sender will fail.
    bool too_many_fds = false;
    if (pipes[1]+1 > FD_SETSIZE) {
      LogError("ForkingFileDescriptors") << "too many file descriptors for multicore job";
      too_many_fds = true;
    }

    //create a thread that sends the units of work to workers
    // we create it after all signals were blocked so that this
    // thread is never interupted by a signal
    MessageSenderToSource sender(childrenSockets, childrenPipes, numberOfSequentialEventsPerChild_);
    boost::thread senderThread(sender);

    if(not too_many_fds) {
      //NOTE: a child could have failed before we got here and even after this call
      // which is why the 'if' is conditional on continueAfterChildFailure_
      possiblyContinueAfterForkChildFailure();
      while(!shutdown_flag && (!child_failed or continueAfterChildFailure_) && (childrenIds.size() != num_children_done)) {
        sigsuspend(&unblockingSigSet);
        possiblyContinueAfterForkChildFailure();
        LogInfo("ForkingAwake") << "woke from sigwait" << std::endl;
      }
    }
    pthread_sigmask(SIG_SETMASK, &oldSigSet, NULL);

    LogInfo("ForkingStopping") << "num children who have already stopped " << num_children_done;
    if(child_failed) {
      LogError("ForkingStopping") << "child failed";
    }
    if(shutdown_flag) {
      LogSystem("ForkingStopping") << "asked to shutdown";
    }

    if(too_many_fds || shutdown_flag || (child_failed && (num_children_done != childrenIds.size()))) {
      LogInfo("ForkingStopping") << "must stop children" << std::endl;
      for(std::vector<pid_t>::iterator it = childrenIds.begin(), itEnd = childrenIds.end();
          it != itEnd; ++it) {
        /* int result = */ kill(*it, SIGUSR2);
      }
      pthread_sigmask(SIG_BLOCK, &blockingSigSet, &oldSigSet);
      while(num_children_done != kMaxChildren) {
        sigsuspend(&unblockingSigSet);
      }
      pthread_sigmask(SIG_SETMASK, &oldSigSet, NULL);
    }
    // The senderThread will notice the pipes die off, one by one.  Once all children are gone, it will exit.
    senderThread.join();
    if(child_failed && !continueAfterChildFailure_) {
      if (child_fail_signal) {
        throw cms::Exception("ForkedChildFailed") << "child process ended abnormally with signal " << child_fail_signal;
      } else if (child_fail_exit_status) {
        throw cms::Exception("ForkedChildFailed") << "child process ended abnormally with exit code " << child_fail_exit_status;
      } else {
        throw cms::Exception("ForkedChildFailed") << "child process ended abnormally for unknown reason";
      }
    }
    if(too_many_fds) {
      throw cms::Exception("ForkedParentFailed") << "hit select limit for number of fds";
    }
    return false;
  }

  std::vector<ModuleDescription const*>
  EventProcessor::getAllModuleDescriptions() const {
    return schedule_->getAllModuleDescriptions();
  }

  int
  EventProcessor::totalEvents() const {
    return schedule_->totalEvents();
  }

  int
  EventProcessor::totalEventsPassed() const {
    return schedule_->totalEventsPassed();
  }

  int
  EventProcessor::totalEventsFailed() const {
    return schedule_->totalEventsFailed();
  }

  void
  EventProcessor::enableEndPaths(bool active) {
    schedule_->enableEndPaths(active);
  }

  bool
  EventProcessor::endPathsEnabled() const {
    return schedule_->endPathsEnabled();
  }

  void
  EventProcessor::getTriggerReport(TriggerReport& rep) const {
    schedule_->getTriggerReport(rep);
  }

  void
  EventProcessor::clearCounters() {
    schedule_->clearCounters();
  }


  std::unique_ptr<statemachine::Machine>
  EventProcessor::createStateMachine() {
    statemachine::FileMode fileMode;
    if(fileMode_.empty()) fileMode = statemachine::FULLMERGE;
    else if(fileMode_ == std::string("NOMERGE")) fileMode = statemachine::NOMERGE;
    else if(fileMode_ == std::string("FULLMERGE")) fileMode = statemachine::FULLMERGE;
    else {
      throw Exception(errors::Configuration, "Illegal fileMode parameter value: ")
        << fileMode_ << ".\n"
        << "Legal values are 'NOMERGE' and 'FULLMERGE'.\n";
    }

    statemachine::EmptyRunLumiMode emptyRunLumiMode;
    if(emptyRunLumiMode_.empty()) emptyRunLumiMode = statemachine::handleEmptyRunsAndLumis;
    else if(emptyRunLumiMode_ == std::string("handleEmptyRunsAndLumis")) emptyRunLumiMode = statemachine::handleEmptyRunsAndLumis;
    else if(emptyRunLumiMode_ == std::string("handleEmptyRuns")) emptyRunLumiMode = statemachine::handleEmptyRuns;
    else if(emptyRunLumiMode_ == std::string("doNotHandleEmptyRunsAndLumis")) emptyRunLumiMode = statemachine::doNotHandleEmptyRunsAndLumis;
    else {
      throw Exception(errors::Configuration, "Illegal emptyMode parameter value: ")
        << emptyRunLumiMode_ << ".\n"
        << "Legal values are 'handleEmptyRunsAndLumis', 'handleEmptyRuns', and 'doNotHandleEmptyRunsAndLumis'.\n";
    }

    auto machine = std::make_unique<statemachine::Machine>(
                                                           this,
                                                           fileMode,
                                                           emptyRunLumiMode);

    machine->initiate();
    return machine;
  }

  bool
  EventProcessor::checkForAsyncStopRequest(StatusCode& returnCode) {
    bool returnValue = false;

    // Look for a shutdown signal
    if(shutdown_flag.load(std::memory_order_acquire)) {
      returnValue = true;
      returnCode = epSignal;
    }
    return returnValue;
  }


  EventProcessor::StatusCode
  EventProcessor::runToCompletion() {

    StatusCode returnCode=epSuccess;
    asyncStopStatusCodeFromProcessingEvents_=epSuccess;
    std::unique_ptr<statemachine::Machine> machine;
    {
      beginJob(); //make sure this was called

      //StatusCode returnCode = epSuccess;
      stateMachineWasInErrorState_ = false;

      // make the services available
      ServiceRegistry::Operate operate(serviceToken_);

      machine = createStateMachine();
      nextItemTypeFromProcessingEvents_=InputSource::IsEvent;
      asyncStopRequestedWhileProcessingEvents_=false;
      try {
        convertException::wrap([&]() {

            InputSource::ItemType itemType;

            while(true) {

              bool more = true;
              if(numberOfForkedChildren_ > 0) {
                size_t size = preg_->size();
                {
                  SendSourceTerminationSignalIfException sentry(actReg_.get());
                  more = input_->skipForForking();
                  sentry.completedSuccessfully();
                }
                if(more) {
                  if(size < preg_->size()) {
                    principalCache_.adjustIndexesAfterProductRegistryAddition();
                  }
                  principalCache_.adjustEventsToNewProductRegistry(preg());
                }
              }
              {
                SendSourceTerminationSignalIfException sentry(actReg_.get());
                itemType = (more ? input_->nextItemType() : InputSource::IsStop);
                sentry.completedSuccessfully();
              }

              FDEBUG(1) << "itemType = " << itemType << "\n";

              if(checkForAsyncStopRequest(returnCode)) {
                actReg_->preSourceEarlyTerminationSignal_(TerminationOrigin::ExternalSignal);
                forceLooperToEnd_ = true;
                machine->process_event(statemachine::Stop());
                forceLooperToEnd_ = false;
                break;
              }

              if(itemType == InputSource::IsEvent) {
                machine->process_event(statemachine::Event());
                if(asyncStopRequestedWhileProcessingEvents_) {
                  forceLooperToEnd_ = true;
                  machine->process_event(statemachine::Stop());
                  forceLooperToEnd_ = false;
                  returnCode = asyncStopStatusCodeFromProcessingEvents_;
                  break;
                }
                itemType = nextItemTypeFromProcessingEvents_;
              }

              if(itemType == InputSource::IsEvent) {
              }
              else if(itemType == InputSource::IsStop) {
                machine->process_event(statemachine::Stop());
              }
              else if(itemType == InputSource::IsFile) {
                machine->process_event(statemachine::File());
              }
              else if(itemType == InputSource::IsRun) {
                machine->process_event(statemachine::Run(input_->reducedProcessHistoryID(), input_->run()));
              }
              else if(itemType == InputSource::IsLumi) {
                machine->process_event(statemachine::Lumi(input_->luminosityBlock()));
              }
              else if(itemType == InputSource::IsSynchronize) {
                //For now, we don't have to do anything
              }
              // This should be impossible
              else {
                throw Exception(errors::LogicError)
                  << "Unknown next item type passed to EventProcessor\n"
                  << "Please report this error to the Framework group\n";
              }
              if(machine->terminated()) {
                break;
              }
            }  // End of loop over state machine events
          }); // convertException::wrap
      } // Try block
      // Some comments on exception handling related to the boost state machine:
      //
      // Some states used in the machine are special because they
      // perform actions while the machine is being terminated, actions
      // such as close files, call endRun, call endLumi etc ...  Each of these
      // states has two functions that perform these actions.  The functions
      // are almost identical.  The major difference is that one version
      // catches all exceptions and the other lets exceptions pass through.
      // The destructor catches them and the other function named "exit" lets
      // them pass through.  On a normal termination, boost will always call
      // "exit" and then the state destructor.  In our state classes, the
      // the destructors do nothing if the exit function already took
      // care of things.  Here's the interesting part.  When boost is
      // handling an exception the "exit" function is not called (a boost
      // feature).
      //
      // If an exception occurs while the boost machine is in control
      // (which usually means inside a process_event call), then
      // the boost state machine destroys its states and "terminates" itself.
      // This already done before we hit the catch blocks below. In this case
      // the call to terminateMachine below only destroys an already
      // terminated state machine.  Because exit is not called, the state destructors
      // handle cleaning up lumis, runs, and files.  The destructors swallow
      // all exceptions and only pass through the exceptions messages, which
      // are tacked onto the original exception below.
      //
      // If an exception occurs when the boost state machine is not
      // in control (outside the process_event functions), then boost
      // cannot destroy its own states.  The terminateMachine function
      // below takes care of that.  The flag "alreadyHandlingException"
      // is set true so that the state exit functions do nothing (and
      // cannot throw more exceptions while handling the first).  Then the
      // state destructors take care of this because exit did nothing.
      //
      // In both cases above, the EventProcessor::endOfLoop function is
      // not called because it can throw exceptions.
      //
      // One tricky aspect of the state machine is that things that can
      // throw should not be invoked by the state machine while another
      // exception is being handled.
      // Another tricky aspect is that it appears to be important to
      // terminate the state machine before invoking its destructor.
      // We've seen crashes that are not understood when that is not
      // done.  Maintainers of this code should be careful about this.

      catch (cms::Exception & e) {
        alreadyHandlingException_ = true;
        terminateMachine(std::move(machine));
        alreadyHandlingException_ = false;
        if (!exceptionMessageLumis_.empty()) {
          e.addAdditionalInfo(exceptionMessageLumis_);
          if (e.alreadyPrinted()) {
            LogAbsolute("Additional Exceptions") << exceptionMessageLumis_;
          }
        }
        if (!exceptionMessageRuns_.empty()) {
          e.addAdditionalInfo(exceptionMessageRuns_);
          if (e.alreadyPrinted()) {
            LogAbsolute("Additional Exceptions") << exceptionMessageRuns_;
          }
        }
        if (!exceptionMessageFiles_.empty()) {
          e.addAdditionalInfo(exceptionMessageFiles_);
          if (e.alreadyPrinted()) {
            LogAbsolute("Additional Exceptions") << exceptionMessageFiles_;
          }
        }
        throw;
      }

      if(machine->terminated()) {
        FDEBUG(1) << "The state machine reports it has been terminated\n";
        machine.reset();
      }

      if(stateMachineWasInErrorState_) {
        throw cms::Exception("BadState")
          << "The boost state machine in the EventProcessor exited after\n"
          << "entering the Error state.\n";
      }

    }
    if(machine.get() != nullptr) {
      terminateMachine(std::move(machine));
      throw Exception(errors::LogicError)
        << "State machine not destroyed on exit from EventProcessor::runToCompletion\n"
        << "Please report this error to the Framework group\n";
    }

    return returnCode;
  }

  void EventProcessor::readFile() {
    FDEBUG(1) << " \treadFile\n";
    size_t size = preg_->size();
    SendSourceTerminationSignalIfException sentry(actReg_.get());

    fb_ = input_->readFile();
    if(size < preg_->size()) {
      principalCache_.adjustIndexesAfterProductRegistryAddition();
    }
    principalCache_.adjustEventsToNewProductRegistry(preg());
    if((numberOfForkedChildren_ > 0) or
       (preallocations_.numberOfStreams()>1 and
        preallocations_.numberOfThreads()>1)) {
      fb_->setNotFastClonable(FileBlock::ParallelProcesses);
    }
    sentry.completedSuccessfully();
  }

  void EventProcessor::closeInputFile(bool cleaningUpAfterException) {
    if (fb_.get() != nullptr) {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      input_->closeFile(fb_.get(), cleaningUpAfterException);
      sentry.completedSuccessfully();
    }
    FDEBUG(1) << "\tcloseInputFile\n";
  }

  void EventProcessor::openOutputFiles() {
    if (fb_.get() != nullptr) {
      schedule_->openOutputFiles(*fb_);
      for_all(subProcesses_, [this](auto& subProcess){ subProcess.openOutputFiles(*fb_); });
    }
    FDEBUG(1) << "\topenOutputFiles\n";
  }

  void EventProcessor::closeOutputFiles() {
    if (fb_.get() != nullptr) {
      schedule_->closeOutputFiles();
      for_all(subProcesses_, [](auto& subProcess){ subProcess.closeOutputFiles(); });
    }
    FDEBUG(1) << "\tcloseOutputFiles\n";
  }

  void EventProcessor::respondToOpenInputFile() {
    for_all(subProcesses_, [this](auto& subProcess){ subProcess.updateBranchIDListHelper(branchIDListHelper_->branchIDLists()); } );
    if (fb_.get() != nullptr) {
      schedule_->respondToOpenInputFile(*fb_);
      for_all(subProcesses_, [this](auto& subProcess) { subProcess.respondToOpenInputFile(*fb_); });
    }
    FDEBUG(1) << "\trespondToOpenInputFile\n";
  }

  void EventProcessor::respondToCloseInputFile() {
    if (fb_.get() != nullptr) {
      schedule_->respondToCloseInputFile(*fb_);
      for_all(subProcesses_, [this](auto& subProcess){ subProcess.respondToCloseInputFile(*fb_); });
    }
    FDEBUG(1) << "\trespondToCloseInputFile\n";
  }

  void EventProcessor::startingNewLoop() {
    shouldWeStop_ = false;
    //NOTE: for first loop, need to delay calling 'doStartingNewLoop'
    // until after we've called beginOfJob
    if(looper_ && looperBeginJobRun_) {
      looper_->doStartingNewLoop();
    }
    FDEBUG(1) << "\tstartingNewLoop\n";
  }

  bool EventProcessor::endOfLoop() {
    if(looper_) {
      ModuleChanger changer(schedule_.get(),preg_.get());
      looper_->setModuleChanger(&changer);
      EDLooperBase::Status status = looper_->doEndOfLoop(esp_->eventSetup());
      looper_->setModuleChanger(nullptr);
      if(status != EDLooperBase::kContinue || forceLooperToEnd_) return true;
      else return false;
    }
    FDEBUG(1) << "\tendOfLoop\n";
    return true;
  }

  void EventProcessor::rewindInput() {
    input_->repeat();
    input_->rewind();
    FDEBUG(1) << "\trewind\n";
  }

  void EventProcessor::prepareForNextLoop() {
    looper_->prepareForNextLoop(esp_.get());
    FDEBUG(1) << "\tprepareForNextLoop\n";
  }

  bool EventProcessor::shouldWeCloseOutput() const {
    FDEBUG(1) << "\tshouldWeCloseOutput\n";
    if(!subProcesses_.empty()) {
      for(auto const& subProcess : subProcesses_) {
        if(subProcess.shouldWeCloseOutput()) {
          return true;
        }
      }
      return false;
    }
    return schedule_->shouldWeCloseOutput();
  }

  void EventProcessor::doErrorStuff() {
    FDEBUG(1) << "\tdoErrorStuff\n";
    LogError("StateMachine")
      << "The EventProcessor state machine encountered an unexpected event\n"
      << "and went to the error state\n"
      << "Will attempt to terminate processing normally\n"
      << "(IF using the looper the next loop will be attempted)\n"
      << "This likely indicates a bug in an input module or corrupted input or both\n";
    stateMachineWasInErrorState_ = true;
  }
  
  void EventProcessor::beginRun(statemachine::Run const& run) {
    RunPrincipal& runPrincipal = principalCache_.runPrincipal(run.processHistoryID(), run.runNumber());
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());

      input_->doBeginRun(runPrincipal, &processContext_);
      sentry.completedSuccessfully();
    }

    IOVSyncValue ts(EventID(runPrincipal.run(), 0, 0),
                    runPrincipal.beginTime());
    if(forceESCacheClearOnNewRun_){
      espController_->forceCacheClear();
    }
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      espController_->eventSetupForInstance(ts);
      sentry.completedSuccessfully();
    }
    EventSetup const& es = esp_->eventSetup();
    if(looper_ && looperBeginJobRun_== false) {
      looper_->copyInfo(ScheduleInfo(schedule_.get()));
      looper_->beginOfJob(es);
      looperBeginJobRun_ = true;
      looper_->doStartingNewLoop();
    }
    {
      typedef OccurrenceTraits<RunPrincipal, BranchActionGlobalBegin> Traits;
      auto globalWaitTask = make_empty_waiting_task();
      globalWaitTask->increment_ref_count();
      beginGlobalTransitionAsync<Traits>(WaitingTaskHolder(globalWaitTask.get()),
                                         *schedule_,
                                         runPrincipal,
                                         ts,
                                         es,
                                         subProcesses_);
      globalWaitTask->wait_for_all();
      if(globalWaitTask->exceptionPtr() != nullptr) {
        std::rethrow_exception(* (globalWaitTask->exceptionPtr()) );
      }
    }
    FDEBUG(1) << "\tbeginRun " << run.runNumber() << "\n";
    if(looper_) {
      looper_->doBeginRun(runPrincipal, es, &processContext_);
    }
    {
      //To wait, the ref count has to be 1+#streams
      auto streamLoopWaitTask = make_empty_waiting_task();
      streamLoopWaitTask->increment_ref_count();
      
      typedef OccurrenceTraits<RunPrincipal, BranchActionStreamBegin> Traits;
      
      beginStreamsTransitionAsync<Traits>(streamLoopWaitTask.get(),
                                         *schedule_,
                                         preallocations_.numberOfStreams(),
                                         runPrincipal,
                                         ts,
                                         es,
                                         subProcesses_);

      streamLoopWaitTask->wait_for_all();
      if(streamLoopWaitTask->exceptionPtr() != nullptr) {
        std::rethrow_exception(* (streamLoopWaitTask->exceptionPtr()) );
      }
    }
    FDEBUG(1) << "\tstreamBeginRun " << run.runNumber() << "\n";
    if(looper_) {
      //looper_->doStreamBeginRun(schedule_->streamID(),runPrincipal, es);
    }
  }

  void EventProcessor::endRun(statemachine::Run const& run, bool cleaningUpAfterException) {
    RunPrincipal& runPrincipal = principalCache_.runPrincipal(run.processHistoryID(), run.runNumber());
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());

      runPrincipal.setEndTime(input_->timestamp());
      runPrincipal.setComplete();
      input_->doEndRun(runPrincipal, cleaningUpAfterException, &processContext_);
      sentry.completedSuccessfully();
    }

    IOVSyncValue ts(EventID(runPrincipal.run(), LuminosityBlockID::maxLuminosityBlockNumber(), EventID::maxEventNumber()),
                    runPrincipal.endTime());
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      espController_->eventSetupForInstance(ts);
      sentry.completedSuccessfully();
    }
    EventSetup const& es = esp_->eventSetup();
    {
      //To wait, the ref count has to be 1+#streams
      auto streamLoopWaitTask = make_empty_waiting_task();
      streamLoopWaitTask->increment_ref_count();
      
      typedef OccurrenceTraits<RunPrincipal, BranchActionStreamEnd> Traits;
      
      endStreamsTransitionAsync<Traits>(streamLoopWaitTask.get(),
                                       *schedule_,
                                       preallocations_.numberOfStreams(),
                                       runPrincipal,
                                       ts,
                                       es,
                                       subProcesses_,
                                       cleaningUpAfterException);
      
      streamLoopWaitTask->wait_for_all();
      if(streamLoopWaitTask->exceptionPtr() != nullptr) {
        std::rethrow_exception(* (streamLoopWaitTask->exceptionPtr()) );
      }
    }
    FDEBUG(1) << "\tstreamEndRun " << run.runNumber() << "\n";
    if(looper_) {
      //looper_->doStreamEndRun(schedule_->streamID(),runPrincipal, es);
    }
    {
      runPrincipal.setAtEndTransition(true);
      typedef OccurrenceTraits<RunPrincipal, BranchActionGlobalEnd> Traits;
      schedule_->processOneGlobal<Traits>(runPrincipal, es, cleaningUpAfterException);
      for_all(subProcesses_, [&runPrincipal, &ts, cleaningUpAfterException](auto& subProcess){subProcess.doEndRun(runPrincipal, ts, cleaningUpAfterException); });
    }
    FDEBUG(1) << "\tendRun " << run.runNumber() << "\n";
    if(looper_) {
      looper_->doEndRun(runPrincipal, es, &processContext_);
    }
  }

  void EventProcessor::beginLumi(ProcessHistoryID const& phid, RunNumber_t run, LuminosityBlockNumber_t lumi) {
    LuminosityBlockPrincipal& lumiPrincipal = principalCache_.lumiPrincipal(phid, run, lumi);
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());

      input_->doBeginLumi(lumiPrincipal, &processContext_);
      sentry.completedSuccessfully();
    }

    Service<RandomNumberGenerator> rng;
    if(rng.isAvailable()) {
      LuminosityBlock lb(lumiPrincipal, ModuleDescription(), nullptr);
      rng->preBeginLumi(lb);
    }

    // NOTE: Using 0 as the event number for the begin of a lumi block is a bad idea
    // lumi blocks know their start and end times why not also start and end events?
    IOVSyncValue ts(EventID(lumiPrincipal.run(), lumiPrincipal.luminosityBlock(), 0), lumiPrincipal.beginTime());
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      espController_->eventSetupForInstance(ts);
      sentry.completedSuccessfully();
    }
    EventSetup const& es = esp_->eventSetup();
    {
      typedef OccurrenceTraits<LuminosityBlockPrincipal, BranchActionGlobalBegin> Traits;
      auto globalWaitTask = make_empty_waiting_task();
      globalWaitTask->increment_ref_count();
      beginGlobalTransitionAsync<Traits>(WaitingTaskHolder(globalWaitTask.get()),
                                         *schedule_,
                                         lumiPrincipal,
                                         ts,
                                         es,
                                         subProcesses_);
      globalWaitTask->wait_for_all();
      if(globalWaitTask->exceptionPtr() != nullptr) {
        std::rethrow_exception(* (globalWaitTask->exceptionPtr()) );
      }
    }
    FDEBUG(1) << "\tbeginLumi " << run << "/" << lumi << "\n";
    if(looper_) {
      looper_->doBeginLuminosityBlock(lumiPrincipal, es, &processContext_);
    }
    {
      //To wait, the ref count has to b 1+#streams
      auto streamLoopWaitTask = make_empty_waiting_task();
      streamLoopWaitTask->increment_ref_count();

      typedef OccurrenceTraits<LuminosityBlockPrincipal, BranchActionStreamBegin> Traits;

      beginStreamsTransitionAsync<Traits>(streamLoopWaitTask.get(),
                                         *schedule_,
                                         preallocations_.numberOfStreams(),
                                         lumiPrincipal,
                                         ts,
                                         es,
                                         subProcesses_);
      streamLoopWaitTask->wait_for_all();
      if(streamLoopWaitTask->exceptionPtr() != nullptr) {
        std::rethrow_exception(* (streamLoopWaitTask->exceptionPtr()) );
      }
    }
    
    FDEBUG(1) << "\tstreamBeginLumi " << run << "/" << lumi << "\n";
    if(looper_) {
      //looper_->doStreamBeginLuminosityBlock(schedule_->streamID(),lumiPrincipal, es);
    }
  }

  void EventProcessor::endLumi(ProcessHistoryID const& phid, RunNumber_t run, LuminosityBlockNumber_t lumi, bool cleaningUpAfterException) {
    LuminosityBlockPrincipal& lumiPrincipal = principalCache_.lumiPrincipal(phid, run, lumi);
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());

      lumiPrincipal.setEndTime(input_->timestamp());
      lumiPrincipal.setComplete();
      input_->doEndLumi(lumiPrincipal, cleaningUpAfterException, &processContext_);
      sentry.completedSuccessfully();
    }
    //NOTE: Using the max event number for the end of a lumi block is a bad idea
    // lumi blocks know their start and end times why not also start and end events?
    IOVSyncValue ts(EventID(lumiPrincipal.run(), lumiPrincipal.luminosityBlock(), EventID::maxEventNumber()),
                    lumiPrincipal.endTime());
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      espController_->eventSetupForInstance(ts);
      sentry.completedSuccessfully();
    }
    EventSetup const& es = esp_->eventSetup();
    {
      //To wait, the ref count has to b 1+#streams
      auto streamLoopWaitTask = make_empty_waiting_task();
      streamLoopWaitTask->increment_ref_count();
      
      typedef OccurrenceTraits<LuminosityBlockPrincipal, BranchActionStreamEnd> Traits;
      
      endStreamsTransitionAsync<Traits>(streamLoopWaitTask.get(),
                                       *schedule_,
                                       preallocations_.numberOfStreams(),
                                       lumiPrincipal,
                                       ts,
                                       es,
                                       subProcesses_,
                                       cleaningUpAfterException);
      streamLoopWaitTask->wait_for_all();
      if(streamLoopWaitTask->exceptionPtr() != nullptr) {
        std::rethrow_exception(* (streamLoopWaitTask->exceptionPtr()) );
      }
    }
    FDEBUG(1) << "\tendLumi " << run << "/" << lumi << "\n";
    if(looper_) {
      //looper_->doStreamEndLuminosityBlock(schedule_->streamID(),lumiPrincipal, es);
    }
    {
      lumiPrincipal.setAtEndTransition(true);
      typedef OccurrenceTraits<LuminosityBlockPrincipal, BranchActionGlobalEnd> Traits;
      schedule_->processOneGlobal<Traits>(lumiPrincipal, es, cleaningUpAfterException);
      for_all(subProcesses_, [&lumiPrincipal, &ts, cleaningUpAfterException](auto& subProcess){	subProcess.doEndLuminosityBlock(lumiPrincipal, ts, cleaningUpAfterException); });
    }
    FDEBUG(1) << "\tendLumi " << run << "/" << lumi << "\n";
    if(looper_) {
      looper_->doEndLuminosityBlock(lumiPrincipal, es, &processContext_);
    }
  }

  statemachine::Run EventProcessor::readRun() {
    if (principalCache_.hasRunPrincipal()) {
      throw edm::Exception(edm::errors::LogicError)
        << "EventProcessor::readRun\n"
        << "Illegal attempt to insert run into cache\n"
        << "Contact a Framework Developer\n";
    }
    auto rp = std::make_shared<RunPrincipal>(input_->runAuxiliary(), preg(), *processConfiguration_, historyAppender_.get(), 0);
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      input_->readRun(*rp, *historyAppender_);
      sentry.completedSuccessfully();
    }
    assert(input_->reducedProcessHistoryID() == rp->reducedProcessHistoryID());
    principalCache_.insert(rp);
    return statemachine::Run(rp->reducedProcessHistoryID(), input_->run());
  }

  statemachine::Run EventProcessor::readAndMergeRun() {
    principalCache_.merge(input_->runAuxiliary(), preg());
    auto runPrincipal =principalCache_.runPrincipalPtr();
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      input_->readAndMergeRun(*runPrincipal);
      sentry.completedSuccessfully();
    }
    assert(input_->reducedProcessHistoryID() == runPrincipal->reducedProcessHistoryID());
    return statemachine::Run(runPrincipal->reducedProcessHistoryID(), input_->run());
  }

  int EventProcessor::readLuminosityBlock() {
    if (principalCache_.hasLumiPrincipal()) {
      throw edm::Exception(edm::errors::LogicError)
        << "EventProcessor::readRun\n"
        << "Illegal attempt to insert lumi into cache\n"
        << "Contact a Framework Developer\n";
    }
    if (!principalCache_.hasRunPrincipal()) {
      throw edm::Exception(edm::errors::LogicError)
        << "EventProcessor::readRun\n"
        << "Illegal attempt to insert lumi into cache\n"
        << "Run is invalid\n"
        << "Contact a Framework Developer\n";
    }
    auto lbp = std::make_shared<LuminosityBlockPrincipal>(input_->luminosityBlockAuxiliary(), preg(), *processConfiguration_, historyAppender_.get(), 0);
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      input_->readLuminosityBlock(*lbp, *historyAppender_);
      sentry.completedSuccessfully();
    }
    lbp->setRunPrincipal(principalCache_.runPrincipalPtr());
    principalCache_.insert(lbp);
    return input_->luminosityBlock();
  }

  int EventProcessor::readAndMergeLumi() {
    principalCache_.merge(input_->luminosityBlockAuxiliary(), preg());
    {
      SendSourceTerminationSignalIfException sentry(actReg_.get());
      input_->readAndMergeLumi(*principalCache_.lumiPrincipalPtr());
      sentry.completedSuccessfully();
    }
    return input_->luminosityBlock();
  }

  void EventProcessor::writeRun(statemachine::Run const& run) {
    schedule_->writeRun(principalCache_.runPrincipal(run.processHistoryID(), run.runNumber()), &processContext_);
    for_all(subProcesses_, [&run](auto& subProcess){ subProcess.writeRun(run.processHistoryID(), run.runNumber()); });
    FDEBUG(1) << "\twriteRun " << run.runNumber() << "\n";
  }

  void EventProcessor::deleteRunFromCache(statemachine::Run const& run) {
    principalCache_.deleteRun(run.processHistoryID(), run.runNumber());
    for_all(subProcesses_, [&run](auto& subProcess){ subProcess.deleteRunFromCache(run.processHistoryID(), run.runNumber()); });
    FDEBUG(1) << "\tdeleteRunFromCache " << run.runNumber() << "\n";
  }

  void EventProcessor::writeLumi(ProcessHistoryID const& phid, RunNumber_t run, LuminosityBlockNumber_t lumi) {
    schedule_->writeLumi(principalCache_.lumiPrincipal(phid, run, lumi), &processContext_);
    for_all(subProcesses_, [&phid, run, lumi](auto& subProcess){ subProcess.writeLumi(phid, run, lumi); });
    FDEBUG(1) << "\twriteLumi " << run << "/" << lumi << "\n";
  }

  void EventProcessor::deleteLumiFromCache(ProcessHistoryID const& phid, RunNumber_t run, LuminosityBlockNumber_t lumi) {
    principalCache_.deleteLumi(phid, run, lumi);
    for_all(subProcesses_, [&phid, run, lumi](auto& subProcess){ subProcess.deleteLumiFromCache(phid, run, lumi); });
    FDEBUG(1) << "\tdeleteLumiFromCache " << run << "/" << lumi << "\n";
  }

  bool EventProcessor::readNextEventForStream(unsigned int iStreamIndex,
                                              std::atomic<bool>* finishedProcessingEvents) {
    if(shouldWeStop()) {
      return false;
    }
    
    if(deferredExceptionPtrIsSet_.load(std::memory_order_acquire)) {
      return false;
    }
    
    if(finishedProcessingEvents->load(std::memory_order_acquire)) {
      return false;
    }

    ServiceRegistry::Operate operate(serviceToken_);
    try {
      //need to use lock in addition to the serial task queue because
      // of delayed provenance reading and reading data in response to
      // edm::Refs etc
      std::lock_guard<std::recursive_mutex> guard(*(sourceMutex_.get()));
      if(not firstEventInBlock_) {
        //The state machine already called input_->nextItemType
        // and found an event. We can't call input_->nextItemType
        // again since it would move to the next transition
        InputSource::ItemType itemType = input_->nextItemType();
        if (InputSource::IsEvent !=itemType) {
          nextItemTypeFromProcessingEvents_ = itemType;
          finishedProcessingEvents->store(true,std::memory_order_release);
          //std::cerr<<"next item type "<<itemType<<"\n";
          return false;
        }
        if((asyncStopRequestedWhileProcessingEvents_=checkForAsyncStopRequest(asyncStopStatusCodeFromProcessingEvents_))) {
          //std::cerr<<"task told to async stop\n";
          actReg_->preSourceEarlyTerminationSignal_(TerminationOrigin::ExternalSignal);
          return false;
        }
      } else {
        firstEventInBlock_ = false;
      }
      readEvent(iStreamIndex);
    } catch (...) {
      bool expected =false;
      if(deferredExceptionPtrIsSet_.compare_exchange_strong(expected,true)) {
        deferredExceptionPtr_ = std::current_exception();

      }
      return false;
    }
    return true;
  }
  
  void EventProcessor::handleNextEventForStreamAsync(WaitingTask* iTask,
                                                              unsigned int iStreamIndex,
                                                              std::atomic<bool>* finishedProcessingEvents)
  {
    auto recursionTask = make_waiting_task(tbb::task::allocate_root(), [this,iTask,iStreamIndex,finishedProcessingEvents](std::exception_ptr const* iPtr) {
      if(iPtr) {
        bool expected = false;
        if(deferredExceptionPtrIsSet_.compare_exchange_strong(expected,true)) {
          deferredExceptionPtr_ = *iPtr;
          {
            WaitingTaskHolder h(iTask);
            h.doneWaiting(*iPtr);
          }
        }
        //the stream will stop now
        iTask->decrement_ref_count();
        return;
      }

      handleNextEventForStreamAsync(iTask, iStreamIndex,finishedProcessingEvents);
    });
      
    sourceResourcesAcquirer_.serialQueueChain().push([this,finishedProcessingEvents,recursionTask,iTask,iStreamIndex]() {
           ServiceRegistry::Operate operate(serviceToken_);

           try {
             if(readNextEventForStream(iStreamIndex, finishedProcessingEvents) ) {
               processEventAsync( WaitingTaskHolder(recursionTask), iStreamIndex);
             } else {
               //the stream will stop now
               tbb::task::destroy(*recursionTask);
               iTask->decrement_ref_count();
             }
           } catch(...) {
             WaitingTaskHolder h(recursionTask);
             h.doneWaiting(std::current_exception());
           }
    });
  }

  void EventProcessor::readAndProcessEvent() {
    if(numberOfForkedChildren_>0) {
      //Have to do something special for forking since
      // after each event the system may have to skip
      // some transitions. This is handled in runToCompletion
      readEvent(0);
      auto eventLoopWaitTask = make_empty_waiting_task();
      eventLoopWaitTask->increment_ref_count();
      processEventAsync(WaitingTaskHolder(eventLoopWaitTask.get()),0);
      eventLoopWaitTask->wait_for_all();
      return;
    }
    nextItemTypeFromProcessingEvents_ = InputSource::IsEvent; //needed for looper
    asyncStopRequestedWhileProcessingEvents_ = false;

    std::atomic<bool> finishedProcessingEvents{false};
    auto finishedProcessingEventsPtr = &finishedProcessingEvents;

    //The state machine already found the event so
    // we have to avoid looking again
    firstEventInBlock_ = true;

    //To wait, the ref count has to b 1+#streams
    auto eventLoopWaitTask = make_empty_waiting_task();
    auto eventLoopWaitTaskPtr = eventLoopWaitTask.get();
    eventLoopWaitTask->increment_ref_count();

    const unsigned int kNumStreams = preallocations_.numberOfStreams();
    unsigned int iStreamIndex = 0;
    for(; iStreamIndex<kNumStreams-1; ++iStreamIndex) {
      eventLoopWaitTask->increment_ref_count();
      tbb::task::enqueue( *make_waiting_task(tbb::task::allocate_root(),[this,iStreamIndex,finishedProcessingEventsPtr,eventLoopWaitTaskPtr](std::exception_ptr const*){
        handleNextEventForStreamAsync(eventLoopWaitTaskPtr,iStreamIndex,finishedProcessingEventsPtr);
      }) );
    }
    eventLoopWaitTask->increment_ref_count();
    eventLoopWaitTask->spawn_and_wait_for_all( *make_waiting_task(tbb::task::allocate_root(),[this,iStreamIndex,finishedProcessingEventsPtr,eventLoopWaitTaskPtr](std::exception_ptr const*){
      handleNextEventForStreamAsync(eventLoopWaitTaskPtr,iStreamIndex,finishedProcessingEventsPtr);
    }));

    //One of the processing threads saw an exception
    if(deferredExceptionPtrIsSet_) {
      std::rethrow_exception(deferredExceptionPtr_);
    }
  }
  void EventProcessor::readEvent(unsigned int iStreamIndex) {
    //TODO this will have to become per stream
    auto& event = principalCache_.eventPrincipal(iStreamIndex);
    StreamContext streamContext(event.streamID(), &processContext_);

    SendSourceTerminationSignalIfException sentry(actReg_.get());
    input_->readEvent(event, streamContext);
    sentry.completedSuccessfully();

    FDEBUG(1) << "\treadEvent\n";
  }

  void EventProcessor::processEventAsync(WaitingTaskHolder iHolder,
                                         unsigned int iStreamIndex) {
    auto pep = &(principalCache_.eventPrincipal(iStreamIndex));
    pep->setLuminosityBlockPrincipal(principalCache_.lumiPrincipalPtr());
    Service<RandomNumberGenerator> rng;
    if(rng.isAvailable()) {
      Event ev(*pep, ModuleDescription(), nullptr);
      rng->postEventRead(ev);
    }
    assert(pep->luminosityBlockPrincipalPtrValid());
    assert(principalCache_.lumiPrincipalPtr()->run() == pep->run());
    assert(principalCache_.lumiPrincipalPtr()->luminosityBlock() == pep->luminosityBlock());
    
    WaitingTaskHolder finalizeEventTask( make_waiting_task(
                    tbb::task::allocate_root(),
                    [this,pep,iHolder](std::exception_ptr const* iPtr) mutable
             {
               ServiceRegistry::Operate operate(serviceToken_);

               //NOTE: If we have a looper we only have one Stream
               if(looper_) {
                 processEventWithLooper(*pep);
               }
               
               FDEBUG(1) << "\tprocessEvent\n";
               pep->clearEventPrincipal();
               if(iPtr) {
                 iHolder.doneWaiting(*iPtr);
               } else {
                 iHolder.doneWaiting(std::exception_ptr());
               }
             }
                                                           )
                                        );
    WaitingTaskHolder afterProcessTask;
    if(subProcesses_.empty()) {
      afterProcessTask = std::move(finalizeEventTask);
    } else {
      //Need to run SubProcesses after schedule has finished
      // with the event
      afterProcessTask = WaitingTaskHolder(
                   make_waiting_task(tbb::task::allocate_root(),
                                     [this,pep,finalizeEventTask] (std::exception_ptr const* iPtr) mutable
      {
         if(not iPtr) {
           ServiceRegistry::Operate operate(serviceToken_);

           //when run with 1 thread, we want to the order to be what
           // it was before. This requires reversing the order since
           // tasks are run last one in first one out
           for(auto& subProcess: boost::adaptors::reverse(subProcesses_)) {
             subProcess.doEventAsync(finalizeEventTask,*pep);
           }
         } else {
           finalizeEventTask.doneWaiting(*iPtr);
         }
       })
                                           );
    }
    
    schedule_->processOneEventAsync(std::move(afterProcessTask),
                                    iStreamIndex,*pep, esp_->eventSetup());

  }

  void EventProcessor::processEventWithLooper(EventPrincipal& iPrincipal) {
    bool randomAccess = input_->randomAccess();
    ProcessingController::ForwardState forwardState = input_->forwardState();
    ProcessingController::ReverseState reverseState = input_->reverseState();
    ProcessingController pc(forwardState, reverseState, randomAccess);
    
    EDLooperBase::Status status = EDLooperBase::kContinue;
    do {
      
      StreamContext streamContext(iPrincipal.streamID(), &processContext_);
      status = looper_->doDuringLoop(iPrincipal, esp_->eventSetup(), pc, &streamContext);
      
      bool succeeded = true;
      if(randomAccess) {
        if(pc.requestedTransition() == ProcessingController::kToPreviousEvent) {
          input_->skipEvents(-2);
        }
        else if(pc.requestedTransition() == ProcessingController::kToSpecifiedEvent) {
          succeeded = input_->goToEvent(pc.specifiedEventTransition());
        }
      }
      pc.setLastOperationSucceeded(succeeded);
    } while(!pc.lastOperationSucceeded());
    if(status != EDLooperBase::kContinue) shouldWeStop_ = true;
  }

  bool EventProcessor::shouldWeStop() const {
    FDEBUG(1) << "\tshouldWeStop\n";
    if(shouldWeStop_) return true;
    if(!subProcesses_.empty()) {
      for(auto const& subProcess : subProcesses_) {
        if(subProcess.terminate()) {
          return true;
        }
      }
      return false;
    }
    return schedule_->terminate();
  }

  void EventProcessor::setExceptionMessageFiles(std::string& message) {
    exceptionMessageFiles_ = message;
  }

  void EventProcessor::setExceptionMessageRuns(std::string& message) {
    exceptionMessageRuns_ = message;
  }

  void EventProcessor::setExceptionMessageLumis(std::string& message) {
    exceptionMessageLumis_ = message;
  }

  bool EventProcessor::alreadyHandlingException() const {
    return alreadyHandlingException_;
  }

  void EventProcessor::terminateMachine(std::unique_ptr<statemachine::Machine> iMachine) {
    if(iMachine.get() != nullptr) {
      if(!iMachine->terminated()) {
        forceLooperToEnd_ = true;
        iMachine->process_event(statemachine::Stop());
        forceLooperToEnd_ = false;
      }
      else {
        FDEBUG(1) << "EventProcess::terminateMachine  The state machine was already terminated \n";
      }
      if(iMachine->terminated()) {
        FDEBUG(1) << "The state machine reports it has been terminated (3)\n";
      }
    }
  }
}
