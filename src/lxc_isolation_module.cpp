#include "lxc_isolation_module.hpp"

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>

#include "foreach.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::make_pair;
using std::max;
using std::ostringstream;
using std::pair;
using std::queue;
using std::string;
using std::vector;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::slave;

namespace {

const int32_t CPU_SHARES_PER_CPU = 1024;
const int32_t MIN_CPU_SHARES = 10;
const int64_t MIN_RSS = 128 * Megabyte;

}


LxcIsolationModule::LxcIsolationModule(Slave* slave)
{
  this->slave = slave;
  reaper = new Reaper(this);
  Process::spawn(reaper);

  // Run a basic check to see whether Linux Container tools are available
  if (system("lxc-version > /dev/null") != 0) {
    LOG(FATAL) << "Could not run lxc-version; make sure Linux Container "
                << "tools are installed";
  }
  // Check that we are root (it might also be possible to create Linux
  // containers without being root, but we can support that later)
  if (getuid() != 0) {
    LOG(FATAL) << "LXC isolation module requires slave to run as root";
  }
}


LxcIsolationModule::~LxcIsolationModule()
{
  // We want to wait until the reaper has completed because it
  // accesses 'this' in order to make callbacks ... deleting 'this'
  // could thus lead to a seg fault!
  Process::post(reaper->getPID(), SHUTDOWN_REAPER);
  Process::wait(reaper);
  delete reaper;
}


void LxcIsolationModule::frameworkAdded(Framework* fw)
{
  infos[fw->id] = new FrameworkInfo();
  infos[fw->id]->lxcExecutePid = -1;
  infos[fw->id]->container = "";
  fw->executorStatus = "No executor running";
}


void LxcIsolationModule::frameworkRemoved(Framework* fw)
{
  if (infos.find(fw->id) != infos.end()) {
    delete infos[fw->id];
    infos.erase(fw->id);
  }
}


void LxcIsolationModule::startExecutor(Framework *fw)
{
  LOG(INFO) << "Starting executor for framework " << fw->id << ": "
            << fw->executorInfo.uri;
  CHECK(infos[fw->id]->lxcExecutePid == -1 && infos[fw->id]->container == "");

  // Get location of Nexus install in order to find nexus-launcher.
  const char *nexusHome = getenv("NEXUS_HOME");
  if (!nexusHome)
    nexusHome = "..";
  string nexusLauncher = string(nexusHome) + "/src/nexus-launcher";

  // Create a name for the container
  ostringstream oss;
  oss << "nexus.slave-" << slave->id << ".framework-" << fw->id;
  string containerName = oss.str();

  infos[fw->id]->container = containerName;
  fw->executorStatus = "Container: " + containerName;

  // Run lxc-execute nexus-launcher using a fork-exec (since lxc-execute
  // does not return until the container is finished). Note that lxc-execute
  // automatically creates the container and will delete it when finished.
  pid_t pid;
  if ((pid = fork()) == -1)
    PLOG(FATAL) << "Failed to fork to launch lxc-execute";

  if (pid) {
    // In parent process
    infos[fw->id]->lxcExecutePid = pid;
    LOG(INFO) << "Started lxc-execute, pid = " << pid;
    int status;
  } else {
    // Set any environment variables given as env.* params in the ExecutorInfo
    const string_map& params = fw->executorInfo.params;
    foreachpair (const string& key, const string& value, params) {
      if (key.find("env.") == 0) {
        const string& var = key.substr(strlen("env."));
        setenv(var.c_str(), value.c_str(), true);
      }
    }

    // Set up Nexus environment variables for launcher.
    setenv("NEXUS_FRAMEWORK_ID", lexical_cast<string>(fw->id).c_str(), 1);
    setenv("NEXUS_EXECUTOR_URI", fw->executorInfo.uri.c_str(), 1);
    setenv("NEXUS_USER", fw->user.c_str(), 1);
    setenv("NEXUS_SLAVE_PID", lexical_cast<string>(slave->self()).c_str(), 1);
    setenv("NEXUS_REDIRECT_IO", slave->local ? "0" : "1", 1);
    setenv("NEXUS_WORK_DIRECTORY", slave->getWorkDirectory(fw->id).c_str(), 1);

    // Run lxc-execute.
    execlp("lxc-execute", "lxc-execute", "-n", containerName.c_str(),
           nexusLauncher.c_str(), (char *) NULL);
    // If we get here, the execl call failed.
    fatalerror("Could not exec lxc-execute");
    // TODO: Exit the slave if this happens
  }
}


void LxcIsolationModule::killExecutor(Framework* fw)
{
  string container = infos[fw->id]->container;
  if (container != "") {
    LOG(INFO) << "Stopping container " << container;
    int ret = shell("lxc-stop -n %s", container.c_str());
    if (ret != 0)
      LOG(ERROR) << "lxc-stop returned " << ret;
    infos[fw->id]->container = "";
    fw->executorStatus = "No executor running";
  }
}


void LxcIsolationModule::resourcesChanged(Framework* fw)
{
  if (infos[fw->id]->container != "") {
    // For now, just try setting the CPUs and memory right away, and kill the
    // framework if this fails.
    // A smarter thing to do might be to only update them periodically in a
    // separate thread, and to give frameworks some time to scale down their
    // memory usage.

    int32_t cpuShares = max(CPU_SHARES_PER_CPU * fw->resources.cpus,
                            MIN_CPU_SHARES);
    if (!setResourceLimit(fw, "cpu.shares", cpuShares)) {
      slave->removeExecutor(fw->id, true);
      return;
    }

    int64_t rssLimit = max(fw->resources.mem, MIN_RSS);
    if (!setResourceLimit(fw, "memory.limit_in_bytes", rssLimit)) {
      slave->removeExecutor(fw->id, true);
      return;
    }
  }
}


bool LxcIsolationModule::setResourceLimit(Framework* fw,
                                          const string& property,
                                          int64_t value)
{
  LOG(INFO) << "Setting " << property << " for framework " << fw->id
            << " to " << value;
  int ret = shell("lxc-cgroup -n %s %s %lld",
                  infos[fw->id]->container.c_str(),
                  property.c_str(),
                  value);
  if (ret != 0) {
    LOG(ERROR) << "Failed to set " << property << " for framework " << fw->id
               << ": lxc-cgroup returned " << ret;
    return false;
  } else {
    return true;
  }
}


int LxcIsolationModule::shell(const char* fmt, ...)
{
  char *cmd;
  FILE *f;
  int ret;
  va_list args;
  va_start(args, fmt);
  if (vasprintf(&cmd, fmt, args) == -1)
    return -1;
  if ((f = popen(cmd, "w")) == NULL)
    return -1;
  ret = pclose(f);
  if (ret == -1)
    LOG(INFO) << "pclose error: " << strerror(errno);
  free(cmd);
  va_end(args);
  return ret;
}


LxcIsolationModule::Reaper::Reaper(LxcIsolationModule* m)
  : module(m)
{}

  
void LxcIsolationModule::Reaper::operator () ()
{
  link(module->slave->getPID());
  while (true) {
    switch (receive(1)) {
    case PROCESS_TIMEOUT: {
      // Check whether any child process has exited
      pid_t pid;
      int status;
      if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
        foreachpair (FrameworkID fid, FrameworkInfo* info, module->infos) {
          if (info->lxcExecutePid == pid) {
            info->container = "";
            info->lxcExecutePid = -1;
            LOG(INFO) << "Telling slave of lost framework " << fid;
            // TODO(benh): This is broken if/when libprocess is parallel!
            module->slave->executorExited(fid, status);
            break;
          }
        }
      }
      break;
    }
    case SHUTDOWN_REAPER:
    case PROCESS_EXIT:
      return;
    }
  }
}
