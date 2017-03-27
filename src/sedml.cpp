#include <string>
#include <iostream>
#include <vector>
#include <map>

#include <sbml/SBMLTypes.h>

#include "utils.hpp"
#include "sedml.hpp"
#include "dataset.hpp"
#include "simulationenginecsim.hpp"
#include "simulationengineget.hpp"
#include "utilityclasses.hpp"
#include "setvaluechange.hpp"

LIBSEDML_CPP_NAMESPACE_USE

static void printStringMap(StringMap& map)
{
    for (auto i = map.begin(); i != map.end(); ++i)
    {
        std::cout << "Key: " << i->first.c_str() << " ==> " << i->second.c_str() << std::endl;
    }
}

static std::string nonEssentialString()
{
    static int counter = 1000;
    std::stringstream ss;
    ss << counter++;
    std::string number(ss.str());
    std::string value("bob_");
    value += number;
    return value;
}

static std::map<std::string, std::string> getAllNamespaces(SedBase* current)
{
    std::map<std::string, std::string> nsList;
    // grab all the namespaces, to use when resolving target xpaths
    while (current)
    {
        const XMLNamespaces* nss = current->getNamespaces();
        if (nss)
        {
            for (int i = 0; i < nss->getNumNamespaces(); ++i)
            {
                std::string prefix = nss->getPrefix(i);
                if (nsList.count(prefix) == 0)
                {
                    // only want to add a namespace if its not already in there
                    nsList[prefix] = nss->getURI(i);
                }
            }
        }
        current = current->getParentSedObject();
    }
    return nsList;
}

class MyRange
{
public:
    std::string id;
    std::vector<double> rangeData;
};

class MyTask
{
public:
    MyTask()
    {
        isRepeatedTask = false;
    }

    /**
     * @brief Execute the given task
     * @param models
     * @param simulations
     * @param dataSets
     * @param masterTaskId The ID of the current top-level task being executed (the parent repeated task)
     * @param changesToApply
     * @return
     */
    int execute(std::map<std::string, MyModel>& models, std::map<std::string, MySimulation>& simulations,
                DataSet& dataSets, const std::string& masterTaskId, bool resetModel,
                std::vector<MySetValueChange>& changesToApply)
    {
        int numberOfErrors = 0;
        if (isRepeatedTask) numberOfErrors = executeRepeated(models, simulations, dataSets, masterTaskId, changesToApply);
        else numberOfErrors = executeSingle(models, simulations, dataSets, masterTaskId, resetModel, changesToApply);
        return numberOfErrors;
    }

    int executeRepeated(std::map<std::string, MyModel>& models, std::map<std::string, MySimulation>& simulations,
                        DataSet& dataSets, const std::string& masterTaskId,
                        std::vector<MySetValueChange>& changesToApply)
    {
        int numberOfErrors = 0;
        // FIXME: a quick and dirty initial implementation of repeated tasks
        const MyRange& r = ranges[masterRangeId];
        int rangeIndex = 0;
        // take a copy of the set value changes that we need to make
        std::vector<MySetValueChange> localSetValueChanges(setValueChanges);
        for (double rangeValue: r.rangeData)
        {
            std::cout << "Execute repeat with master range (" << masterRangeId << ") value: " << rangeValue << std::endl;
            // update the set value changes to have the current values
            for (MySetValueChange& svc: localSetValueChanges)
            {
                const MyRange& rr = ranges[svc.rangeId];
                svc.currentRangeValue = rr.rangeData[rangeIndex];
                std::cout << "Setting range: " << svc.rangeId << "; to value: " << svc.currentRangeValue << std::endl;
            }
            // and add them to the existing set of changes
            localSetValueChanges.insert(localSetValueChanges.end(), changesToApply.begin(), changesToApply.end());
            // and then execute the sub tasks
            for (MyTask& st: subTasks) numberOfErrors += st.execute(models, simulations, dataSets, masterTaskId,
                                                                    resetModel, localSetValueChanges);
            ++rangeIndex;
        }
        return numberOfErrors;
    }

    int executeSingle(std::map<std::string, MyModel>& models, std::map<std::string, MySimulation>& simulations,
                      DataSet& dataSets, const std::string& masterTaskId, bool resetModel,
                      std::vector<MySetValueChange>& changesToApply)
    {
        int numberOfErrors = 0;
        std::cout << "\n\nExecuting: " << id.c_str() << std::endl;
        std::cout << "\tsimulation = " << simulationReference.c_str() << std::endl;
        std::cout << "\tmodel = " << modelReference.c_str() << std::endl;
        std::cout << "\tnumber of changes to apply = " << changesToApply.size() << std::endl;
        const MyModel& model = models[modelReference];
        const MySimulation& simulation = simulations[simulationReference];
        std::vector<std::string> outputVariables;
        if (simulation.isCsim())
        {
            std::cout << "\trunning simulation task using CSim..." << std::endl;
            // we need to keep track of exisiting CSim objects so that when executing repeated tasks
            // the already created and initialised CSim can be used.
            SimulationEngineCsim* csim;
            if (csimList.count(id))
            {
                csim = csimList[id];
                if (resetModel) csim->resetSimulator(true);
            }
            else
            {
                csim = new SimulationEngineCsim();
                csim->loadModel(model.source);
                // keep track of the data arrays to store the simulation results
                for (auto& di: dataSets)
                {
                    MyData& d = di.second;
                    // we only want variables relevant to this task (the master task)
                    for (auto& variables: d.variables)
                    {
                        MyVariable& v = variables.second;
                        if (v.taskReference == masterTaskId)
                        {
                            outputVariables.push_back(variables.first);
                            std::cout << "\tAdding variable: "
                                      << variables.first
                                      << "; to the outputs for this task."
                                      << std::endl;
                            csim->addOutputVariable(v);
                        }
                    }
                }
                // we also need to access the variables for setting changes as inputs
                for (MySetValueChange& change: changesToApply)
                {
                    if (change.modelReference == modelReference)
                    {
                        csim->addInputVariable(change);
                    }
                }
                // initialise the engine
                if (csim->instantiateSimulation() != 0)
                {
                    std::cerr << "Error instantiating the simulation?" << std::endl;
                    return -1;
                }
                csimList[id] = csim;
            }
            // apply relevant changes
            for (const MySetValueChange& change: changesToApply)
            {
                std::cout << "Change to apply: " << std::endl;
                if (change.modelReference == modelReference)
                {
                    std::cout << "Applying set value change, value: " << change.currentRangeValue << std::endl;
                    csim->applySetValueChange(change);
                }
            }
            // initialise the simulation
            csim->initialiseSimulation(simulation, simulation.initialTime, simulation.startTime);
            std::cout << "got to here 2345" << std::endl;
            // set up the results capture
            std::vector<std::vector<double>*> results;
            for (auto di = dataSets.begin(); di != dataSets.end(); ++di)
            {
                MyData& d = di->second;
                for (auto& variables: d.variables)
                {
                    MyVariable& v = variables.second;
                    if (v.taskReference == masterTaskId)
                        results.push_back(&(v.data));
                }
            }
            std::vector<double> stepResults = csim->getOutputValues();
            int r = 0;
            for (auto j = stepResults.begin(); j != stepResults.end(); ++j, ++r)
                results[r]->push_back(*j);
            std::cout << "Got to here 1234" << std::endl;
            double dt = (simulation.endTime - simulation.startTime) / simulation.numberOfPoints;
            double time = simulation.startTime;
            for (int i = 1; i <= simulation.numberOfPoints; ++i, time += dt)
            {
                if (csim->simulateModelOneStep(dt) == 0)
                {
                    stepResults = csim->getOutputValues();
                    r = 0;
                    for (auto j = stepResults.begin(); j != stepResults.end(); ++j, ++r)
                        results[r]->push_back(*j);
                }
                else
                {
                    std::cerr << "Error in simulation at time = " << time << std::endl;
                    break;
                }
            }
        }
        else if (simulation.isGet())
        {
            std::cout << "\trunning simulation task using GET..." << std::endl;
            SimulationEngineGet get;
            get.loadModel(model.source);
            int columnIndex = 1;
            for (auto di = dataSets.begin(); di != dataSets.end(); ++di, ++columnIndex)
            {
                const MyData& d = di->second;
                // we only want variables relevant to this task
                for (auto& variables: d.variables)
                {
                    const MyVariable& v = variables.second;
                    if (v.taskReference == this->id)
                    {
                        outputVariables.push_back(variables.first);
                        std::cout << "\tAdding variable: " << variables.first
                                  << "; to the outputs for this task."
                                  << std::endl;
                        get.addOutputVariable(v, columnIndex);
                    }
                }
            }
            get.initialiseSimulation();
            get.getOutputValues();
        }
        else
        {
            std::cerr << "Unknown type of simulation?" << std::endl;
            ++numberOfErrors;
        }
        return numberOfErrors;
    }

    std::string id;
    std::string name;
    std::string modelReference;
    std::string simulationReference;
    bool isRepeatedTask;
    bool resetModel;
    std::string masterRangeId;
    std::map<std::string, MyRange> ranges;
    std::vector<MyTask> subTasks;
    std::vector<MySetValueChange> setValueChanges;
    std::map<std::string, SimulationEngineCsim*> csimList;

    ~MyTask()
    {
        for (auto i = csimList.begin(); i != csimList.end(); ++i) delete i->second;
    }
};


class MyReport
{
public:
    MyReport(const SedReport* sedReport)
    {
        sed = sedReport;
        id = sed->getId();
        for (unsigned int i = 0; i < sed->getNumDataSets(); ++i)
        {
            const SedDataSet* dataSet = sed->getDataSet(i);
            MyData d;
            // these strings are not required in the SED-ML document, so need to handle them being absent
            d.id = dataSet->isSetId() ? dataSet->getId() : nonEssentialString();
            d.label = dataSet->getLabel(); // empty string if no label set
            d.dataReference = dataSet->getDataReference();
            dataSets[d.id] = d;
        }
    }

    int resolveDataSets(SedDocument* doc)
    {
        int numberOfErrors = 0;
        for (auto i = dataSets.begin(); i != dataSets.end(); ++i)
        {
            std::cout << "DataSet " << i->first.c_str() << ":" << std::endl;
            MyData& d = i->second;
            std::cout << "\tdata reference: " << d.dataReference.c_str() << std::endl;
            std::cout << "\tlabel: " << (d.label == "" ? "no label" : d.label.c_str()) << std::endl;
            SedDataGenerator* dg = doc->getDataGenerator(d.dataReference);
            if (dg->getNumVariables() < 1)
            {
                std::cerr << "We need at least one variable, sorry!" << std::endl;
                ++numberOfErrors;
            }
            else
            {
                for (unsigned int vc=0; vc < dg->getNumVariables(); ++vc)
                {
                    SedVariable* v = dg->getVariable(vc);
                    MyVariable var;
                    var.target = v->getTarget();
                    var.taskReference = v->getTaskReference();
                    var.namespaces = getAllNamespaces(v);
                    std::cout << "\t\tVariable " << v->getId()
                              << ": target=" << var.target
                              << "; task=" << var.taskReference << std::endl;
                    printStringMap(var.namespaces);
                    d.variables[v->getId()] = var;
                }
            }
            for (unsigned int pc=0; pc < dg->getNumParameters(); ++pc)
            {
                SedParameter* p = dg->getParameter(pc);
                std::cout << "\t\tParameter " << p->getId()
                          << ": value=" << p->getValue() << std::endl;
                d.parameters[p->getId()] = p->getValue();
            }
        }
        return numberOfErrors;
    }

    int resolveTasks(SedDocument* doc)
    {
        int numberOfErrors = 0;
        for (auto i = dataSets.begin(); i != dataSets.end(); ++i)
        {
            std::cout << "DataSet " << i->first.c_str() << ":" << std::endl;
            MyData& d = i->second;
            for (auto& variables: d.variables)
            {
                MyVariable& v = variables.second;
                const SedTask* task = doc->getTask(v.taskReference);
                MyTask t;
                t.id = task->getId();
                if (tasks.count(t.id) == 0)
                {
                    std::cout << "Adding task: " << t.id << " to the execution manifest" << std::endl;
                    resolveTask(t, task);
                    tasks[t.id] = t;
                }
                else std::cout << "Task (" << t.id << ") already in the execution manifest" << std::endl;
            }
        }
        return numberOfErrors;
    }

    int resolveTask(MyTask& t, const SedTask* task)
    {
        int numberOfErrors = 0;
        const SedRepeatedTask* repeat = dynamic_cast<const SedRepeatedTask*>(task);
        if (repeat != NULL)
        {
            t.isRepeatedTask = true;
            numberOfErrors += resolveRepeatedTask(t, repeat);
        }
        else
        {
            t.name = task->getName();
            t.modelReference = task->getModelReference();
            t.simulationReference = task->getSimulationReference();
        }
        return numberOfErrors;
    }

    int resolveRepeatedTask(MyTask& repeat, const SedRepeatedTask* sedRepeat)
    {
        int numberOfErrors = 0;
        repeat.resetModel = sedRepeat->getResetModel();
        repeat.masterRangeId = sedRepeat->getRangeId();

        for (unsigned int i = 0; i < sedRepeat->getNumRanges(); ++i)
        {
          const SedRange* current = sedRepeat->getRange(i);
          const SedFunctionalRange* functional = dynamic_cast<const SedFunctionalRange*>(current);
          const SedVectorRange* vrange = dynamic_cast<const SedVectorRange*>(current);
          const SedUniformRange* urange = dynamic_cast<const SedUniformRange*>(current);
          if (functional != NULL)
          {
              numberOfErrors++;
              std::cerr << "Functional ranges not yet supported";
          }
          else if (vrange != NULL)
          {
              MyRange range;
              range.id = vrange->getId();
              range.rangeData = std::vector<double>(vrange->getValues());
              repeat.ranges[range.id] = range;
          }
          else if (urange != NULL)
          {
              MyRange range;
              range.id = urange->getId();
              std::string t = urange->getType();
              if (t == "log")
              {
                  numberOfErrors++;
                  std::cerr << "Uniform ranges of type 'log' are not yet supported" << std::endl;
              }
              else
              {
                  double start = urange->getStart();
                  double end = urange->getEnd();
                  double n = urange->getNumberOfPoints();
                  double step = (end - start) / n;
                  for (int i = 0; i <= n; ++i)
                  {
                      double value = start + i * step;
                      range.rangeData.push_back(value);
                  }
                  repeat.ranges[range.id] = range;
              }
          }
        }

        for (unsigned int i = 0; i < sedRepeat->getNumTaskChanges(); ++i)
        {
            const SedSetValue* current = sedRepeat->getTaskChange(i);
            // FIXME: for now assume that if a range attribute is present we simply want to assign the current range value
            // to the given target. Will ignore the math.
            MySetValueChange vc;
            vc.rangeId = current->getRange();
            vc.modelReference = current->getModelReference();
            vc.targetXpath = current->getTarget();
            // grab all the namespaces to use when resolving the target xpaths
            const SedBase* c = current;
            while (c)
            {
                const XMLNamespaces* nss = c->getNamespaces();
                if (nss)
                {
                    for (int i = 0; i < nss->getNumNamespaces(); ++i)
                    {
                        std::string prefix = nss->getPrefix(i);
                        if (vc.namespaces.count(prefix) == 0)
                        {
                            // only want to add a namespace if its not already in there
                            vc.namespaces[prefix] = nss->getURI(i);
                        }
                    }
                }
                c = c->getParentSedObject();
            }
            repeat.setValueChanges.push_back(vc);
        }
        for (unsigned int i = 0; i < sedRepeat->getNumSubTasks(); ++i)
        {
          const SedSubTask* current = sedRepeat->getSubTask(i);
          const SedTask* task = sedRepeat->getSedDocument()->getTask(current->getTask());
          MyTask st;
          st.id = task->getId();
          std::cout << "Adding sub task: " << st.id.c_str() << " to the execution manifest" << std::endl;
          resolveTask(st, task);
          if (current->isSetOrder())
          {
              std::cerr << "\n\n****** Sorry, specifying the order is not yet supported and we ignore it\n" << std::endl;
          }
          repeat.subTasks.push_back(st);
        }
        return numberOfErrors;
    }

    int resolveModel(const SedModel* model, const std::string& baseUri)
    {
        int numberOfErrors = 0;
        MyModel m;
        m.id = model->getId();
        if (models.count(m.id) == 0)
        {
            std::cout << "Adding model: " << m.id.c_str() << " to the execution manifest" << std::endl;
            std::string language = model->getLanguage();
            // we can only handle CellML models.
            if (language.find("cellml"))
            {
                m.name = model->getName(); // empty string is ok
                // FIXME: assuming here that the source is always a cellml model, but could be a reference to
                // another model? or would that be a modelReference?
                m.source = buildAbsoluteUri(model->getSource(), baseUri);
                std::cout << "\tModel source: " << model->getSource().c_str() << std::endl;
                std::cout << "\tModel URL: " << m.source.c_str() << std::endl;
                models[m.id] = m;
            }
            else
            {
                std::cerr << "Sorry, we can only handle CellML models." << std::endl;
                ++numberOfErrors;
            }
        }
        else std::cout << "Model (" << m.id.c_str() << ") is already in the execution manifest" << std::endl;
        return numberOfErrors;
    }

    int resolveRepeatedTaskModels(const MyTask& task, const SedDocument* doc, const std::string& baseUri)
    {
        int numberOfErrors = 0;
        for (const MyTask& st: task.subTasks)
        {
            if (st.isRepeatedTask) numberOfErrors += resolveRepeatedTaskModels(st, doc, baseUri);
            else
            {
                // normal task
                const SedModel* model = doc->getModel(st.modelReference);
                numberOfErrors += resolveModel(model, baseUri);
            }
        }
        return numberOfErrors;
    }

    int resolveModels(SedDocument* doc, const std::string& baseUri)
    {
        int numberOfErrors = 0;
        for (auto i = tasks.begin(); i != tasks.end(); ++i)
        {
            MyTask& task = i->second;
            if (task.isRepeatedTask) numberOfErrors += resolveRepeatedTaskModels(task, doc, baseUri);
            else
            {
                SedModel* model = doc->getModel(task.modelReference);
                numberOfErrors += resolveModel(model, baseUri);
            }
        }
        return numberOfErrors;
    }

    int resolveSimulation(const SedSimulation* simulation)
    {
        int numberOfErrors = 0;
        if (simulation->getTypeCode() == SEDML_SIMULATION_UNIFORMTIMECOURSE)
        {
            MySimulation s;
            s.id = simulation->getId();
            if (simulations.count(s.id) == 0)
            {
                std::cout << "Adding simulation: " << s.id.c_str() << " to the execution manifest" << std::endl;
                const SedUniformTimeCourse* tc = static_cast<const SedUniformTimeCourse*>(simulation);
                const SedAlgorithm* alg = tc->getAlgorithm();
                std::string kisaoId = alg->getKisaoID();
                if ((kisaoId == "KISAO:0000019") || (kisaoId == "KISAO:0000030"))
                {
                    // CVODE integration, we can handle that with CSim
                    // FIXME: with CSim-v2 we can now do more, but this will do to get
                    // things working.
                    s.setSimulationTypeCsim(kisaoId);
                    s.initialTime = tc->getInitialTime();
                    s.startTime = tc->getOutputStartTime();
                    s.endTime = tc->getOutputEndTime();
                    s.numberOfPoints = tc->getNumberOfPoints();
                    // check for any parameters
                    unsigned int np = alg->getNumAlgorithmParameters();
                    for (unsigned int i = 0; i < np; ++i)
                    {
                        const SedAlgorithmParameter* ap = alg->getAlgorithmParameter(i);
                        const std::string& apki = ap->getKisaoID();
                        if (apki == "KISAO:0000211")
                        {
                            // absolute tolerance
                            s.absoluteTolerance = std::stod(ap->getValue());
                            std::cout << "resolveSimulation: setting absolute tolerance = "
                                      << s.absoluteTolerance << std::endl;
                        }
                        else if (apki == "KISAO:0000209")
                        {
                            // relative tolerance
                            s.relativeTolerance = std::stod(ap->getValue());
                            std::cout << "resolveSimulation: setting relative tolerance = "
                                      << s.relativeTolerance << std::endl;
                        }
                        else if (apki == "KISAO:0000467")
                        {
                            // maximum step size
                            s.maximumStepSize = std::stod(ap->getValue());
                            std::cout << "resolveSimulation: setting max step size = "
                                      << s.maximumStepSize << std::endl;
                        }
                        else if (apki == "KISAO:0000415")
                        {
                            // maximum number of steps
                            s.maximumNumberOfSteps = std::stod(ap->getValue());
                            std::cout << "resolveSimulation: setting max number of steps = "
                                      << s.maximumNumberOfSteps << std::endl;
                        }
                    }
                    simulations[s.id] = s;
                }
                else if ((kisaoId == "KISAO:0000000") && alg->isSetAnnotation())
                {
                    // FIXME: this all needs to be namespace aware?!
                    // need to check the annotation to see what to do.
                    XMLNode* annotation = alg->getAnnotation();
                    if (annotation->getNumChildren() != 1)
                    {
                        std::cerr << "Can't handle custom algorithm:\n";
                        std::cerr << annotation->toXMLString() << std::endl;
                        ++numberOfErrors;
                    }
                    else
                    {
                        XMLNode& csimGetSimulator = annotation->getChild(0);
                        if (!csimGetSimulator.hasAttr("method"))
                        {
                            std::cerr << "Missing method attribute on:\n"
                                      << csimGetSimulator.toXMLString() << std::endl;
                            ++numberOfErrors;
                        }
                        else
                        {
                            s.setSimulationTypeGet(csimGetSimulator.getAttrValue("method"));
                            s.initialTime = tc->getInitialTime();
                            s.startTime = tc->getOutputStartTime();
                            s.endTime = tc->getOutputEndTime();
                            s.numberOfPoints = tc->getNumberOfPoints();
                            simulations[s.id] = s;
                        }
                    }
                }
            }
            else std::cout << "Simulation (" << s.id.c_str() << ") already in execution manifest." << std::endl;
        }
        else
        {
            std::cerr << "Unable to handle simulations that are not uniform time courses" << std::endl;
            ++numberOfErrors;
        }
        return numberOfErrors;
    }

    int resolveRepeatedTaskSimulations(const MyTask& task, const SedDocument* doc)
    {
        int numberOfErrors = 0;
        for (const MyTask& st: task.subTasks)
        {
            if (st.isRepeatedTask) numberOfErrors += resolveRepeatedTaskSimulations(st, doc);
            else
            {
                // normal task
                const SedSimulation* simulation = doc->getSimulation(st.simulationReference);
                numberOfErrors += resolveSimulation(simulation);
            }
        }
        return numberOfErrors;
    }

    int resolveSimulations(SedDocument* doc)
    {
        int numberOfErrors = 0;
        for (auto i = tasks.begin(); i != tasks.end(); ++i)
        {
            MyTask& task = i->second;
            if (task.isRepeatedTask)
            {
                numberOfErrors += resolveRepeatedTaskSimulations(task, doc);
            }
            else
            {
                // normal task
                SedSimulation* simulation = doc->getSimulation(task.simulationReference);
                numberOfErrors += resolveSimulation(simulation);
            }
        }
        return numberOfErrors;
    }

    int execute()
    {
        // FIXME: this will re-execute tasks that occur in more than one report.
        int numberOfErrors = 0;
        for (auto i = tasks.begin(); i != tasks.end(); ++i)
        {
            MyTask& t = i->second;
            std::vector<MySetValueChange> changes;
            numberOfErrors += t.execute(models, simulations, dataSets, t.id, false, changes);
        }
        return numberOfErrors;
    }

    int serialise(std::ostream& os)
    {
        int numberOfErrors = 0;
        int first = 0, minDataSize = -1;
        for (const auto& ds: dataSets)
        {
            MyData d = ds.second;
            if (first) os << ",";
            else first = 1;
            os << d.label;
#ifdef FIX_SERIALISATION
            int dataLength = d.data.size();
            if ((minDataSize < 0) || (dataLength < minDataSize)) minDataSize = dataLength;
#endif
        }
        os << std::endl;
        // FIXME: for now assume that all datasets have the same number of results?
        for (int i = 0; i < minDataSize; ++i)
        {
            first = 0;
            for (const auto& ds: dataSets)
            {
                MyData d = ds.second;
                if (first) os << ",";
                else first = 1;
#ifdef FIX_SERIALISATION
                os << d.data[i];
#endif
            }
            os << std::endl;
        }
        return numberOfErrors;
    }

    const SedReport* sed;
    std::string id;
    DataSet dataSets;
    std::map<std::string, MyTask> tasks;
    std::map<std::string, MyModel> models;
    std::map<std::string, MySimulation> simulations;
};

class MyReportList : public std::vector<MyReport>
{
public:
    int resolveTasks(SedDocument* doc, const std::string& baseUri)
    {
        int numberOfErrors = 0;
        for (auto i = begin(); i != end(); ++i)
        {
            numberOfErrors += i->resolveDataSets(doc);
            numberOfErrors += i->resolveTasks(doc);
            numberOfErrors += i->resolveModels(doc, baseUri);
            numberOfErrors += i->resolveSimulations(doc);
        }
        return numberOfErrors;
    }

    int execute()
    {
        int numberOfErrors = 0;
        for (auto i = begin(); i != end(); ++i)
        {
            numberOfErrors += i->execute();
        }
        return numberOfErrors;
    }

    int serialise(std::ostream& os)
    {
        int numberOfErrors = 0;
        for (auto i = begin(); i != end(); ++i)
        {
            numberOfErrors += i->serialise(os);
        }
        return numberOfErrors;
    }
};

Sedml::Sedml() : mSed(NULL), mReports(NULL), mExecutionPerformed(false)
{
}

Sedml::~Sedml()
{
    if (mSed) delete mSed;
    if (mReports) delete mReports;
}

int Sedml::parseFromString(const std::string &xmlDocument)
{
    mSed = readSedMLFromString(xmlDocument.c_str());
    int numErrors = mSed->getErrorLog()->getNumFailsWithSeverity(LIBSEDML_SEV_ERROR);
    if (numErrors > 0)
    {
        std::cerr << "Error loading SED-ML document: " << xmlDocument.c_str() << std::endl;
        std::cerr << mSed->getErrorLog()->toString();
        delete mSed;
        mSed = NULL;
        return numErrors;
    }
    if (mSed->getNumErrors() > 0)
    {
      std::cout << "Warnings: " << mSed->getErrorLog()->toString() << std::endl;
    }

    return 0;
}

int Sedml::buildExecutionManifest(const std::string& baseUri)
{
    int numberOfErrors = 0;
    if (mReports) delete mReports;
    for (unsigned int i = 0; i < mSed->getNumOutputs(); ++i)
    {
      SedOutput* current = mSed->getOutput(i);
      switch(current->getTypeCode())
      {
        case SEDML_OUTPUT_REPORT:
        {
          if (!mReports) mReports = new MyReportList();
          mReports->push_back(MyReport(static_cast<SedReport*>(current)));
          break;
        }
        case SEDML_OUTPUT_PLOT2D:
        {
          SedPlot2D* p = static_cast<SedPlot2D*>(current);
          std::cout << "\t[unsupported] Plot2d id=" << current->getId() << " numCurves=" << p->getNumCurves() << std::endl;
          ++numberOfErrors;
          break;
        }
        case SEDML_OUTPUT_PLOT3D:
        {
          SedPlot3D* p = static_cast<SedPlot3D*>(current);
          std::cout << "\t[unsupported] Plot3d id=" << current->getId() << " numSurfaces=" << p->getNumSurfaces() << std::endl;
          ++numberOfErrors;
          break;
        }
        default:
          std::cout << "\tEncountered unknown output " << current->getId() << std::endl;
          ++numberOfErrors;
          break;
      }
    }
    if (numberOfErrors) return numberOfErrors;
    if (mReports)
    {
        // we have some outputs that we can handle, so make sure we have all the information that we need
        // to start configuring and running simulations.
        numberOfErrors = mReports->resolveTasks(mSed, baseUri);
    }

    return numberOfErrors;
}

int Sedml::execute()
{
    int numberOfErrors = 0;
    if (mReports) numberOfErrors = mReports->execute();
    mExecutionPerformed = true;
    return numberOfErrors;
}

int Sedml::serialiseReports(std::ostream& os)
{
    int numberOfErrors = 0;
    if (mExecutionPerformed)
    {
        if (mReports) numberOfErrors += mReports->serialise(os);
    }
    else
    {
        std::cerr << "You need to execute the simulation tasks before serialising the reports?" << std::endl;
        numberOfErrors++;
    }
    return numberOfErrors;
}

int Sedml::checkBob()
{
    int numberOfErrors = 0;
    // first check we can handle the simulations required of us
    std::cout << "The document has " << mSed->getNumSimulations() << " simulation(s)." << std::endl;
    for (unsigned int i = 0; i < mSed->getNumSimulations(); ++i)
    {
      SedSimulation* current = mSed->getSimulation(i);
      switch(current->getTypeCode())
      {
         case SEDML_SIMULATION_UNIFORMTIMECOURSE:
         {
            SedUniformTimeCourse* tc = static_cast<SedUniformTimeCourse*>(current);
            const SedAlgorithm* alg = tc->getAlgorithm();
            std::string kisaoId = alg->getKisaoID();
            if (kisaoId == "KISAO:0000019")
            {
                // CVODE integration, we can handle that
                std::cout << "UniformTimecourse CVODE integration:\n";
                std::cout << "\tid = " << tc->getId() << "\n\tinterval = " << tc->getOutputStartTime() << "-"
                          << tc->getOutputEndTime() << "\n\tnumPoints = " << tc->getNumberOfPoints() << std::endl;
            }
            else if ((kisaoId == "KISAO:0000000") && alg->isSetAnnotation())
            {
                // FIXME: this all needs to be namespace aware?!

                // need to check the annotation to see what to do.
                XMLNode* annotation = alg->getAnnotation();
                if (annotation->getNumChildren() < 1)
                {
                    std::cerr << "Can't handle custom algorithm:\n";
                    std::cerr << annotation->toXMLString() << std::endl;
                    return -31;
                }
                XMLNode& csimGetSimulator = annotation->getChild(0);
                if (!csimGetSimulator.hasAttr("method"))
                {
                    std::cerr << "Missing method attribute on:\n"
                              << csimGetSimulator.toXMLString() << std::endl;
                    return -32;
                }
                std::string getSimulatorMethod = csimGetSimulator.getAttrValue("method");
                std::cout << "UniformTimecourse GET Simulator simulation:\n"
                          << "\tid = " << tc->getId() << "\n\tinterval = " << tc->getOutputStartTime() << "-"
                          << tc->getOutputEndTime() << "\n\tnumPoints = " << tc->getNumberOfPoints()
                          << "\n\tGET method = " << getSimulatorMethod.c_str() << std::endl;
            }
            break;
         }
         default:
            std::cout << "\tUncountered unknown simulation. " << current->getId() << std::endl;
            ++numberOfErrors;
            break;
      }
    }
    std::cout << std::endl;

    // check for models we can handle and keep track of them
    std::cout << "The document has " << mSed->getNumModels() << " model(s)." << std::endl;
    for (unsigned int i = 0; i < mSed->getNumModels(); ++i)
    {
      SedModel* current = mSed->getModel(i);
      std::string modelLanguage = current->getLanguage();
      std::cout << "\tModel id=" << current->getId() << " language=" << current->getLanguage() << " source=" << current->getSource() << " numChanges=" << current->getNumChanges() << std::endl;
      if (modelLanguage.find("cellml"))
      {
          std::cout << "\tWe can handle CellML models of all versions." << std::endl;
      }
      else
      {
          std::cout << "\tWe are unable to handle models of type: " << modelLanguage.c_str() << std::endl;
          ++numberOfErrors;
      }
      if (current->getNumChanges() > 0)
      {
          std::cout << "\tModel changes are currently unsupported" << std::endl;
          ++numberOfErrors;
      }
    }
    std::cout << std::endl;

    std::cout << "The document has " << mSed->getNumTasks() << " task(s)." << std::endl;
    for (unsigned int i = 0; i < mSed->getNumTasks(); ++i)
    {
        SedTask* current = mSed->getTask(i);
        SedRepeatedTask* repeat = dynamic_cast<SedRepeatedTask*>(current);
        if (repeat != NULL)
        {
            std::cout << "\tRepeated tasks not yet supported, sorry!" << std::endl;
            ++numberOfErrors;
        }
        else
            std::cout << "\tTask id=" << current->getId() << " model=" << current->getModelReference() << " sim=" << current->getSimulationReference() << std::endl;
    }
    std::cout << std::endl;

    std::cout << "The document has " << mSed->getNumDataGenerators() << " datagenerators(s)." << std::endl;
    for (unsigned int i = 0; i < mSed->getNumDataGenerators(); ++i)
    {
      SedDataGenerator* current = mSed->getDataGenerator(i);
      std::cout << "\tDG id=" << current->getId() << " math=" << SBML_formulaToString(current->getMath()) << std::endl;
      for (unsigned int j = 0; j < current->getNumVariables(); ++j)
      {
          SedVariable* v = current->getVariable(j);
          std::string id = v->isSetId() ? v->getId().c_str() : "(no id)";
          std::string target = v->isSetTarget() ? v->getTarget() : "(no target)";
          std::string taskRef = v->isSetTaskReference() ? v->getTaskReference() : "(no task reference?)";
          std::cout << "\t\tVariable " << id.c_str() << ": target=" << target.c_str()
               << "; task=" << taskRef.c_str() << std::endl;
      }
    }

    std::cout << std::endl;
    std::cout << "The document has " << mSed->getNumOutputs() << " output(s)." << std::endl;
    for (unsigned int i = 0; i < mSed->getNumOutputs(); ++i)
    {
      SedOutput* current = mSed->getOutput(i);
      switch(current->getTypeCode())
      {
        case SEDML_OUTPUT_REPORT:
        {
          SedReport* r = static_cast<SedReport*>(current);
          std::cout << "\tReport id=" << current->getId() << " numDataSets=" << r->getNumDataSets() << std::endl;
          break;
        }
        case SEDML_OUTPUT_PLOT2D:
        {
          SedPlot2D* p = static_cast<SedPlot2D*>(current);
          std::cout << "\t[unsupported] Plot2d id=" << current->getId() << " numCurves=" << p->getNumCurves() << std::endl;
          ++numberOfErrors;
          break;
        }
        case SEDML_OUTPUT_PLOT3D:
        {
          SedPlot3D* p = static_cast<SedPlot3D*>(current);
          std::cout << "\t[unsupported] Plot3d id=" << current->getId() << " numSurfaces=" << p->getNumSurfaces() << std::endl;
          ++numberOfErrors;
          break;
        }
        default:
          std::cout << "\tEncountered unknown output " << current->getId() << std::endl;
          break;
      }
    }

    if (numberOfErrors)
    {
        std::cerr << "There are things we can't handle, exiting" << std::endl;
        return -100 * numberOfErrors;
    }
    return 0;
}
