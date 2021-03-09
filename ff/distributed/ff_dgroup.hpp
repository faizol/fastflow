#ifndef FF_DGROUP_H
#define FF_DGROUP_H

#include <ff/ff.hpp>
#include <getopt.h>
#include <iostream>
#include <fstream>
#include <map>
#include <exception>
#include "ff_wrappers.hpp"
#include "ff_dreceiver.hpp"
#include "ff_dsender.hpp"

#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

using namespace ff;

class dGroup;

class dGroups {
public:

    static dGroups* Instance(){
        if (i == nullptr)
            i = new dGroups();
        return i;
    }

    void parseConfig();

    void addGroup(std::string label, dGroup* g){ groups.insert(make_pair(label, g));}

    int size(){ return groups.size();}

    void setConfigFile(std::string f){this->configFilePath = f;}

    void setRunningGroup(std::string g){this->runningGroup = g;}

    dGroup* getRunningGroup(){return this->groups[this->runningGroup];}

protected:
    dGroups() : groups(), configFilePath(), runningGroup() {
        // costruttore
    }

private:
    inline static dGroups* i = nullptr;
    std::map<std::string, dGroup*> groups;
    std::string configFilePath;
    std::string runningGroup;

    // helper class to parse config file Json
    struct G {
        std::string name;
        std::string address;
        int port;
        std::vector<std::string> Oconn;

        template <class Archive>
        void load( Archive & ar ){
            ar(cereal::make_nvp("name", name));
            
            try {
                std::string endpoint;
                ar(cereal::make_nvp("endpoint", endpoint)); std::vector endp(split(endpoint, ':'));
                address = endp[0]; port = std::stoi(endp[1]);
            } catch (cereal::Exception&) {ar.setNextName(nullptr);}

            try {
                ar(cereal::make_nvp("OConn", Oconn));
            } catch (cereal::Exception&) {ar.setNextName(nullptr);}
        }
    };

    static inline std::vector<std::string> split (const std::string &s, char delim) {
        std::vector<std::string> result;
        std::stringstream ss (s);
        std::string item;

        while (getline (ss, item, delim))
            result.push_back (item);

        return result;
    }


    static int expectedInputConnections(std::string groupName, std::vector<G>& groups){
        int result = 0;
        for (const G& g : groups)
            if (g.name != groupName)
                for (const std::string& conn : g.Oconn)
                    if (conn == groupName)  result++;
        return result;
    }
};

enum IOTypes { IN, OUT };

template <IOTypes>
class MySet {
private:
    dGroup* group;

    struct ForwarderNode : ff_node{ 
        void* svc(void* input){return input;}
    };
public:
    MySet(dGroup* group): group(group){ }

    template <typename Tin, typename Tout>
    MySet& operator<<(ff_node_t<Tin, Tout>*) {}

    template <typename Tin, typename Tout>
    MySet& operator<<(ff_minode_t<Tin, Tout>*) {}

    template <typename Tin, typename Tout>
    MySet& operator<<(ff_monode_t<Tin, Tout>*) {}

    template <typename Tin, typename Tout>
    MySet& operator<<=(ff_node_t<Tin, Tout>*) {}

    bool check_inout(ff_node* node);
};

class dGroups;

class dGroup : public ff_farm {

    friend class MySet<IN>;
    friend class MySet<OUT>;
private:
    ff_node * parentStructure;

    ff_endpoint endpoint;
    std::vector<ff_endpoint> destinations;
    int expectedInputConnections;

    /**
     * Key: reference to original node
     * Value: pair of [reference to wrapper, serialization_required] 
     **/
    std::map<ff_node*, std::pair<ff_node*, bool>> in_, out_;
    std::map<ff_node*, ff_node*> inout_;

    bool isSource(){return in_.empty() && inout_.empty();}
    bool isSink(){return out_.empty() && inout_.empty();}

public:
    dGroup(ff_node* parent, std::string label): parentStructure(parent), endpoint(), destinations(), expectedInputConnections(0), in(this), out(this){
        dGroups::Instance()->addGroup(label, this);
    }

    static bool isIncludedIn(const ff::svector<ff_node*>& firstSet, std::vector<ff_node*>& secondSet){
        for (const ff_node* n : firstSet)
            if (std::find(secondSet.begin(), secondSet.end(), n) == secondSet.end())
                return false;
        return true;
    }

    bool replaceWrapper(const ff::svector<ff_node*>& list, std::map<ff_node*, std::pair<ff_node*, bool>>& wrappers_){
        for (ff_node* node : list){
            ff_node* parentBB = getBB(this->parentStructure, node);
            if (parentBB != nullptr){
                
                ff_node* wrapped = wrappers_[node].first;
                
                if (parentBB->isPipe()){
                    reinterpret_cast<ff_pipeline*>(parentBB)->change_node(node, wrapped, true);
                    continue;
                }

                if (parentBB->isAll2All()){
                    reinterpret_cast<ff_a2a*>(parentBB)->change_node(node, wrapped, true);
                    continue;
                }

                if (parentBB->isComp()){
                    reinterpret_cast<ff_comb*>(parentBB)->change_node(node, wrapped, true);
                    continue;
                }

                return false;
            }
            return false;
        }
        return true;
    }

    ff_node* getOriginal(ff_node* wrapper){
        auto resultI = std::find_if(this->in_.begin(), this->in_.end(), [&](const std::pair<ff_node*, std::pair<ff_node*, bool>> &pair){return pair.second.first == wrapper;});
        if (resultI != this->in_.end()) return resultI->first;
        auto resultII = std::find_if(this->inout_.begin(), this->inout_.end(), [&](const std::pair<ff_node*, ff_node*> &pair){return pair.second == wrapper;});
        if (resultII != this->inout_.end()) return resultII->first;

        return nullptr;
    }

    static inline bool isSeq(ff_node* n){return (!n->isAll2All() && !n->isComp() && !n->isFarm() && !n->isOFarm() && !n->isPipe());}

    bool processBB(ff_node* bb, std::vector<ff_node*> in_C, std::vector<ff_node*> out_C){
        if (isSeq(bb)){
            ff::svector<ff_node*> svectBB(1); svectBB.push_back(bb);
            if (isSource() && this->out_.find(bb) != this->out_.end() && replaceWrapper(svectBB, this->out_)){
                this->add_workers({this->out_[bb].first});
                return true;
            }

            if (isSink() && this->in_.find(bb) != this->in_.end() && replaceWrapper(svectBB, this->in_)){
                this->add_workers({this->in_[bb].first});
                return true;
            }

            return false;
        }
        
        ff::svector<ff_node*> in_nodes, out_nodes;
        bb->get_in_nodes(in_nodes);
        
        if (!isSource() && !isIncludedIn(in_nodes, in_C))
                return false;

        bb->get_out_nodes(out_nodes);
        
        if (!isSink() && !isIncludedIn(out_nodes, out_C))
                return false;
        
        if ((isSource() || replaceWrapper(in_nodes, this->in_)) && (isSink() || replaceWrapper(out_nodes, this->out_))){
            this->add_workers({bb}); // here the bb is already modified with the wrapper
            return true;
        }

        return false;
    }

    static bool isStageOf(ff_node* n, ff_pipeline* p){
        for (const ff_node* s : p->getStages())
            if (s == n) return true;

        return false;
    }

    static int getInputIndexOfNode(ff_node* bb, ff_node* wrapper, ff_node* original){
        if (bb->isAll2All()){
            ff_a2a* a2a = (ff_a2a*) bb;
            int index = 0;
            for (ff_node* n : a2a->getFirstSet()){
                ff::svector<ff_node*> inputs; n->get_in_nodes(inputs);
                for (const ff_node* input : inputs){
                    if (input == wrapper || input == original)
                        return index;
                    index++;
                }
            }

            index = 0;
            for (ff_node* n : a2a->getSecondSet()){
                ff::svector<ff_node*> inputs; n->get_in_nodes(inputs);
                for (ff_node* input : inputs)
                    if (input == wrapper || input == original) 
                        return index; 
                    else index++;
            }
        }

        int index = 0;
        ff::svector<ff_node*> inputs; bb->get_in_nodes(inputs);
        for (ff_node* input : inputs)
            if (input == wrapper || input == original) 
                return index; 
            else index++;

        return 0;
    }

    std::map<int, int> buildRoutingTable(ff_node* level1BB){
        std::map<int, int> routingTable;
        int localIndex = 0;
        for (ff_node* inputBB : this->getWorkers()){
            ff::svector<ff_node*> inputs; inputBB->get_in_nodes(inputs);
            for (ff_node* input : inputs){
                routingTable[getInputIndexOfNode(level1BB, input, getOriginal(input))] = localIndex;
                localIndex++;
            }
                //routingTable[getInputIndexOfNode(level1BB, reinterpret_cast<Wrapper*>(input)->getOriginal())] = localIndex++;
        }
        return routingTable;
    }



    int buildFarm(ff_pipeline* basePipe = nullptr){ // chimato dalla run & wait della main pipe 

        // find the 1 level builiding block which containes the group (level 1 BB means a BB whoch is a stage in the main piepline)
        ff_node* level1BB = this->parentStructure;
        while(!isStageOf(level1BB, basePipe)){
            level1BB = getBB(basePipe, level1BB);
            if (!level1BB || level1BB == basePipe) throw FF_Exception("A group were created from a builiding block not included in the Main Pipe! :(");
        }

        
        std::vector<ff_node*> in_C, out_C;
        for (const auto& pair : this->in_) in_C.push_back(pair.first);
        for (const auto& pair : this->out_) out_C.push_back(pair.first);


        if (this->parentStructure->isPipe())
           processBB(this->parentStructure, in_C, out_C);


        if (this->parentStructure->isAll2All()){
            ff_a2a * a2a = (ff_a2a*) this->parentStructure;

            if (!processBB(a2a, in_C, out_C)){ // if the user has not wrapped the whole a2a, expan its sets

                for(ff_node* bb : a2a->getFirstSet())
                    processBB(bb, in_C, out_C);
                
                for(ff_node* bb : a2a->getSecondSet())
                    processBB(bb, in_C, out_C);
            }

        }
        
        // in/out nodes left to be added to the farm. The next lines does it
        for (const auto& pair : this->inout_){
            this->add_workers({pair.second});
        }

        if (this->getNWorkers() == 0)
            return -1;

      
        // create receiver
        if (!isSource()){
            std::cout << "Creating the receiver!" << std::endl;
            this->add_emitter(new ff_dreceiver(0 , this->endpoint, this->expectedInputConnections, buildRoutingTable(level1BB))); // set right parameters HERE!!
        }
        // create sender
        if (!isSink()){
            std::cout << "Creating the sender!" << std::endl;
            this->add_collector(new ff_dsender(0, this->destinations), true);
        }
       

        std::cout << "Built a farm of " << this->getNWorkers() << " workers!" << std::endl;
        // call the base class (ff_farm)'s prepare
        return 0;
    }

    void print(){
        for(auto it = inout_.cbegin(); it != inout_.cend(); ++it)
        {
            std::cout << it->first << " " << it->second << std::endl;
        }
    }

    ff_node* getWrapper(ff_node* n){
        return this->inout_[n];
    }

    int run(bool skip_init=false){
        buildFarm();
        return ff_farm::run(skip_init);
    }

    int run(ff_pipeline* baseBB, bool skip_init=false) {
        // parsare il config File
        dGroups* groups_ = dGroups::Instance();
        groups_->parseConfig();
        
        buildFarm(baseBB);

        //std::cout << "Called run of the group!" << std::endl;
        return ff_farm::run(skip_init);
    }

    int wait(){return ff_farm::wait();}

    int run_and_wait_end() {
        if (run()<0) return -1;
        if (ff_farm::wait()<0) return -1;
        return 0;
    }


    void setEndpoint(const std::string address, const int port){
        this->endpoint.address = address;
        this->endpoint.port = port;
    }

    ff_endpoint getEndpoint(){return this->endpoint;}

    void setDestination(ff_endpoint e){ this->destinations.push_back(std::move(e));}

    void setExpectedInputConnections(int connections){this->expectedInputConnections = connections;}
    
    MySet<IN> in;
    MySet<OUT> out;
};


/**
 * If the user uses << operator -> serialization is used 
 * If the user uses <<= operator -> NO serialization is used
 **/

template<>
template<typename Tin, typename Tout>
MySet<IN>& MySet<IN>::operator<<(ff_node_t<Tin, Tout>* node){
    /*if (condizione){
        error("Errore!");
        throw 
    }*/
    
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->out_.extract(node);
    if (!handle.empty()) // the node is edge also in its output
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<true, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<true, false, Tin, Tout>(node, 1, true, nullptr, ((WrapperOUT<false, Tin, Tout>*)handle.mapped().first)->getTransform())});
    else
        this->group->in_.insert({node, {new WrapperIN<true, Tin, Tout>(node, true), true}});

    return *this;
}

template<>
template<typename Tin, typename Tout>
MySet<IN>& MySet<IN>::operator<<(ff_minode_t<Tin, Tout>* node){
    /*if (condizione){
        error("Errore!");
        throw 
    }*/
    
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->out_.extract(node);
    if (!handle.empty()) // the node is edge also in its output
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<true, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<true, false, Tin, Tout>(node, 1, true, nullptr, reinterpret_cast<WrapperOUT<false, Tin, Tout>*>(reinterpret_cast<ff_comb*>(handle.mapped().first)->getFirst())->getTransform())});
    else
        this->group->in_.insert({node, {new WrapperIN<true, Tin, Tout>(node, true), true}});

    return *this;
}

template<>
template<typename Tin, typename Tout>
MySet<IN>& MySet<IN>::operator<<(ff_monode_t<Tin, Tout>* node){
    /*if (condizione){
        error("Errore!");
        throw 
    }*/
    
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->out_.extract(node);
    if (!handle.empty()) // the node is edge also in its output
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<true, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<true, false, Tin, Tout>(node, 1, true, nullptr, ((WrapperOUT<false, Tin, Tout>*)handle.mapped().first)->getTransform())});
    else {
        ff_comb* combine = new ff_comb(new WrapperIN<true, Tin>(new ForwarderNode, true), node, true);
        this->group->in_.insert({node, {combine, true}});
    }

    return *this;
}

template<>
template<typename Tin, typename Tout>
MySet<OUT>& MySet<OUT>::operator<<(ff_node_t<Tin, Tout>* node){
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->in_.extract(node);
    if (!handle.empty()) // the node is edge also in its input
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<true, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<false, true, Tin, Tout>(node, 1, true, ((WrapperIN<false, Tin, Tout>*)handle.mapped().first)->getFinalizer())});
    else
        this->group->out_.insert({node, {new WrapperOUT<true, Tin, Tout>(node, true), true}});

    return *this;
}

template<>
template<typename Tin, typename Tout>
MySet<OUT>& MySet<OUT>::operator<<(ff_minode_t<Tin, Tout>* node){
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->in_.extract(node);
    if (!handle.empty()) // the node is edge also in its input
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<true, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<false, true, Tin, Tout>(node, 1, true, ((WrapperIN<false, Tin, Tout>*)handle.mapped().first)->getFinalizer())});
    else {
        ff_comb* combine = new ff_comb(node, new WrapperOUT<true, Tout>(new ForwarderNode, true), false, true);
        this->group->out_.insert({node, {combine, true}});
    }

    return *this;
}

template<>
template<typename Tin, typename Tout>
MySet<OUT>& MySet<OUT>::operator<<(ff_monode_t<Tin, Tout>* node){
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->in_.extract(node);
    if (!handle.empty()) // the node is edge also in its input
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<true, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<false, true, Tin, Tout>(node, 1, true, reinterpret_cast<WrapperIN<false, Tin, Tout>*>(reinterpret_cast<ff_comb*>(handle.mapped().first)->getLast())->getFinalizer())});
    else
        this->group->out_.insert({node, {new WrapperOUT<true, Tin, Tout>(node, true), true}});

    return *this;
}


template<>
template<typename Tin, typename Tout>
MySet<IN>& MySet<IN>::operator<<=(ff_node_t<Tin, Tout>* node){
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->out_.extract(node);
    if (!handle.empty()) // the node is edge also in its output
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*)new WrapperINOUT<false, true, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<false, false, Tin, Tout>(node, 1, true, nullptr, ((WrapperOUT<false, Tin, Tout>*)handle.mapped().first)->getTransform())});
    else
        this->group->in_.insert({node, {new WrapperIN<false, Tin, Tout>(node, true), false}});

    return *this;
}

template<>
template<typename Tin, typename Tout>
MySet<OUT>& MySet<OUT>::operator<<=(ff_node_t<Tin, Tout>* node){
    if (check_inout(node)) return *this; // the node is already processed in input and output, just skip it!

    auto handle = this->group->in_.extract(node);
    if (!handle.empty()) // the node is edge also in its input
        this->group->inout_.insert({node, handle.mapped().second ? (ff_node*) new WrapperINOUT<true, false, Tin, Tout>(node, true) : (ff_node*)new WrapperINOUT<false, false, Tin, Tout>(node, 1, true, ((WrapperIN<false, Tin, Tout>*)handle.mapped().first)->getFinalizer())});
    else
        this->group->out_.insert({node, {new WrapperOUT<false, Tin, Tout>(node, true), false}});

    return *this;
}

template<IOTypes T>
bool MySet<T>::check_inout(ff_node* node){
        return this->group->inout_.find(node) != this->group->inout_.end();
    }


void dGroups::parseConfig(){
        if (this->configFilePath.empty()) throw FF_Exception("Config file not defined!");

        std::ifstream is(this->configFilePath);

        if (!is) throw FF_Exception("Unable to open configuration file for the program!");
        
        cereal::JSONInputArchive ari(is);

        std::vector<G> parsedGroups;

        try {
            ari(cereal::make_nvp("groups", parsedGroups));
        } catch (const cereal::Exception& e){
            std::cerr <<  e.what();
            exit(EXIT_FAILURE);
        }

        for(G& g : parsedGroups)
            if (groups.find(g.name) != groups.end())
                groups[g.name]->setEndpoint(g.address, g.port);
            else {
                std::cout << "Cannot find group: " << g.name << std::endl;
                throw FF_Exception("A specified group in the configuration file has not been implemented! :(");
            }

        for(G& g : parsedGroups){
            dGroup* groupRef = groups[g.name];
            for(std::string& conn : g.Oconn)
                if (groups.find(conn) != groups.end())
                    groupRef->setDestination(groups[conn]->getEndpoint());
                else throw FF_Exception("A specified destination has a wrong name! :(");
            
            groupRef->setExpectedInputConnections(expectedInputConnections(g.name, parsedGroups)); 
        }

    }

    void DFF_Init(int &argc, char **&argv){
        int c;
        std::string groupName, configFile;

        while (1){
            static struct option long_options[] =
                {
                    {"DFF_GName", required_argument, 0, 'n'},
                    {"DFF_Config", required_argument, 0, 'c'},
                    {0, 0, 0, 0}
                };

            /* getopt_long stores the option index here. */
            int option_index = 0;
            c = getopt_long(argc, argv, "", long_options, &option_index);

            /* Detect the end of the options. */
            if (c == -1) break;

            switch (c){
            case 0:
                /* If this option set a flag, do nothing else now. */
                if (long_options[option_index].flag != 0)
                    break;
                printf("option %s", long_options[option_index].name);
                if (optarg)
                    printf(" with arg %s", optarg);
                printf("\n");
                break;

            case 'c':
                configFile = std::string(optarg);
                break;

            case 'n':
                groupName = std::string(optarg);
                break;

            case '?':
                /* getopt_long already printed an error message. */
                break;

            default:
                abort();
            }
        }

        if (configFile.empty()){
            std::cerr << "Config file not passed as argument!" << std::endl;
            abort();
        }

        if (groupName.empty()){
            std::cerr << "Group not passed as argument!" << std::endl;
            abort();
        }

        dGroups::Instance()->setConfigFile(configFile);
        dGroups::Instance()->setRunningGroup(groupName);

        // if other arguments are passed, they are preserved!
        if (optind <= argc){
            optind--;
            char *exeName = argv[0];
            argc -= optind;
            argv += optind;
            argv[0] = exeName;
        }
    }




// redefinition of createGroup methods for ff_a2a and ff_pipeline
dGroup& ff::ff_a2a::createGroup(std::string name){
    dGroup * g = new dGroup(this, std::move(name));
    return *g;
}

dGroup& ff::ff_pipeline::createGroup(std::string name){
    dGroup * g = new dGroup(this, std::move(name));
    return *g;
}

#endif