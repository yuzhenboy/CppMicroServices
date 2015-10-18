/*=============================================================================

  Library: CppMicroServices

  Copyright (c) German Cancer Research Center,
    Division of Medical and Biological Informatics

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

=============================================================================*/

#include "usFrameworkFactory.h"
#include "usFramework.h"
#include "usGetBundleContext.h"
#include "usBundleContext.h"
#include "usBundle.h"
#include "usAny.h"

#include "usShellService.h"

#include "linenoise.h"
#include "optionparser.h"

#include <iostream>

using namespace us;

#define US_SHELL_PROG_NAME "usShell"

enum  OptionIndex { UNKNOWN, HELP, LOAD_BUNDLE };
const option::Descriptor usage[] =
{
  {UNKNOWN,      0, "" , ""    , option::Arg::None, "USAGE: " US_SHELL_PROG_NAME " [options]\n\n"
                                                    "Options:" },
  {HELP,         0, "h" , "help",option::Arg::None, "  --help, -h  \tPrint usage and exit." },
  {LOAD_BUNDLE,  0, "l", "load", option::Arg::Optional, "  --load, -l  \tLoad bundle." },
  {UNKNOWN,      0, "" ,  ""   , option::Arg::None, "\nExamples:\n"
                                                    "  " US_SHELL_PROG_NAME " --load /home/user/libmybundle.so\n" },
  {0,0,0,0,0,0}
 };

static ShellService* g_ShellService = NULL;

void shellCompletion(const char* buf, linenoiseCompletions* lc)
{
  if (g_ShellService == NULL || buf == NULL) return;

  g_ShellService->GetCompletions(buf);
  std::vector<std::string> completions = g_ShellService->GetCompletions(buf);
  for (std::vector<std::string>::const_iterator iter = completions.begin(),
       iterEnd = completions.end(); iter != iterEnd; ++iter)
  {
    linenoiseAddCompletion(lc, iter->c_str());
  }
}

int main(int argc, char** argv)
{
  argc -= (argc > 0);
  argv += (argc > 0); // skip program name argv[0] if present
  option::Stats stats(usage, argc, argv);
  std::unique_ptr<option::Option[]> options(new option::Option[stats.options_max]);
  std::unique_ptr<option::Option[]> buffer(new option::Option[stats.buffer_max]);
  option::Parser parse(usage, argc, argv, options.get(), buffer.get());

  if (parse.error()) return 1;

  if (options[HELP])
  {
    option::printUsage(std::cout, usage);
    return 0;
  }

  linenoiseSetCompletionCallback(shellCompletion);

  FrameworkFactory factory;
  Framework* framework = factory.NewFramework(std::map<std::string, std::string>());
  framework->Start();
  BundleContext* context = framework->GetBundleContext();

  try
  {
    std::vector<Bundle*> bundles;
    for (option::Option* opt = options[LOAD_BUNDLE]; opt; opt = opt->next())
    {
      if (opt->arg == nullptr) continue;
      std::cout << "Installing " << opt->arg << std::endl;
      bundles.push_back(context->InstallBundle(opt->arg));
    }
    for (auto bundle : bundles)
    {
      bundle->Start();
    }
  }
  catch (const std::exception& e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  ShellService* shellService = NULL;
  ServiceReference<ShellService> ref = context->GetServiceReference<ShellService>();
  if (ref)
  {
    shellService = context->GetService(ref);
  }

  if (shellService == NULL)
  {
    std::cerr << "Shell service not available" << std::endl;
    return EXIT_FAILURE;
  }

  g_ShellService = shellService;

  char* line = NULL;
  while((line = linenoise("us> ")) != NULL)
  {
    /* Do something with the string. */
    if (line[0] != '\0' && line[0] != '/')
    {
      linenoiseHistoryAdd(line); /* Add to the history. */
      //linenoiseHistorySave("history.txt"); /* Save the history on disk. */
    }
    shellService->ExecuteCommand(line);
    free(line);
    std::cout << std::endl;
  }
}