/*
  Copyright 2013, 2014, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 IRIS AS
  Copyright 2014 STATOIL ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED
#define OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED


#include <sys/utsname.h>

#include <opm/simulators/ParallelFileMerger.hpp>

#include <opm/autodiff/BlackoilModelEbos.hpp>
#include <opm/autodiff/NewtonIterationBlackoilSimple.hpp>
#include <opm/autodiff/NewtonIterationBlackoilCPR.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterleaved.hpp>
#include <opm/autodiff/MissingFeatures.hpp>
#include <opm/autodiff/moduleVersion.hpp>
#include <opm/autodiff/ExtractParallelGridInformationToISTL.hpp>
#include <opm/autodiff/RedistributeDataHandles.hpp>
#include <opm/autodiff/SimulatorFullyImplicitBlackoilEbos.hpp>

#include <opm/core/props/satfunc/RelpermDiagnostics.hpp>

#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/common/OpmLog/EclipsePRTLog.hpp>
#include <opm/common/OpmLog/LogUtil.hpp>

#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/Parser/Parser.hpp>
#include <opm/parser/eclipse/Parser/ParseContext.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/IOConfig/IOConfig.hpp>
#include <opm/parser/eclipse/EclipseState/InitConfig/InitConfig.hpp>
#include <opm/parser/eclipse/EclipseState/checkDeck.hpp>

#if HAVE_DUNE_FEM
#include <dune/fem/misc/mpimanager.hh>
#else
#include <dune/common/parallel/mpihelper.hh>
#endif

namespace Opm
{
    // The FlowMain class is the ebos based black-oil simulator.
    template <class TypeTag>
    class FlowMainEbos
    {
        enum FileOutputValue{
            //! \brief No output to files.
            OUTPUT_NONE = 0,
            //! \brief Output only to log files, no eclipse output.
            OUTPUT_LOG_ONLY = 1,
            //! \brief Output to all files.
            OUTPUT_ALL = 3
        };

    public:
        typedef typename GET_PROP(TypeTag, MaterialLaw)::EclMaterialLawManager MaterialLawManager;
        typedef typename GET_PROP_TYPE(TypeTag, Simulator) EbosSimulator;
        typedef typename GET_PROP_TYPE(TypeTag, ThreadManager) EbosThreadManager;
        typedef typename GET_PROP_TYPE(TypeTag, Grid) Grid;
        typedef typename GET_PROP_TYPE(TypeTag, GridView) GridView;
        typedef typename GET_PROP_TYPE(TypeTag, Problem) Problem;
        typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
        typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;

        typedef Opm::SimulatorFullyImplicitBlackoilEbos<TypeTag> Simulator;
        typedef typename Simulator::ReservoirState ReservoirState;
        typedef typename Simulator::OutputWriter OutputWriter;

        /// This is the main function of Flow.
        /// It runs a complete simulation, with the given grid and
        /// simulator classes, based on user command-line input.  The
        /// content of this function used to be in the main() function of
        /// flow.cpp.
        int execute(int argc, char** argv)
        {
            try {
                setupParallelism();
                printStartupMessage();
                const bool ok = setupParameters(argc, argv);
                if (!ok) {
                    return EXIT_FAILURE;
                }

                setupEbosSimulator();
                setupOutput();
                setupLogging();
                printPRTHeader();
                runDiagnostics();
                setupOutputWriter();
                setupLinearSolver();
                createSimulator();

                // Run.
                auto ret =  runSimulator();

                mergeParallelLogFiles();

                return ret;
            }
            catch (const std::exception &e) {
                std::ostringstream message;
                message  << "Program threw an exception: " << e.what();

                if( output_cout_ )
                {
                    // in some cases exceptions are thrown before the logging system is set
                    // up.
                    if (OpmLog::hasBackend("STREAMLOG")) {
                        OpmLog::error(message.str());
                    }
                    else {
                        std::cout << message.str() << "\n";
                    }
                }

                return EXIT_FAILURE;
            }
        }

    protected:
        void setupParallelism()
        {
            // determine the rank of the current process and the number of processes
            // involved in the simulation. MPI must have already been initialized here.
#if HAVE_MPI
            MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank_);
            int mpi_size;
            MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
#else
            mpi_rank_ = 0;
            const int mpi_size = 1;
#endif
            output_cout_ = ( mpi_rank_ == 0 );
            must_distribute_ = ( mpi_size > 1 );

#ifdef _OPENMP
            // OpenMP setup.
            if (!getenv("OMP_NUM_THREADS")) {
                // Default to at most 4 threads, regardless of
                // number of cores (unless ENV(OMP_NUM_THREADS) is defined)
                int num_cores = omp_get_num_procs();
                int num_threads = std::min(4, num_cores);
                omp_set_num_threads(num_threads);
            }
            // omp_get_num_threads() only works as expected within a parallel region.
            const int num_omp_threads = omp_get_max_threads();
            if (mpi_size == 1) {
                std::cout << "OpenMP using " << num_omp_threads << " threads." << std::endl;
            } else {
                std::cout << "OpenMP using " << num_omp_threads << " threads on MPI rank " << mpi_rank_ << "." << std::endl;
            }
#endif
        }

        // Print startup message if on output rank.
        void printStartupMessage()
        {

            if (output_cout_) {
                const int lineLen = 70;
                const std::string version = moduleVersionName();
                const std::string banner = "This is flow "+version;
                const int bannerPreLen = (lineLen - 2 - banner.size())/2;
                const int bannerPostLen = bannerPreLen + (lineLen - 2 - banner.size())%2;
                std::cout << "**********************************************************************\n";
                std::cout << "*                                                                    *\n";
                std::cout << "*" << std::string(bannerPreLen, ' ') << banner << std::string(bannerPostLen, ' ') << "*\n";
                std::cout << "*                                                                    *\n";
                std::cout << "* Flow is a simulator for fully implicit three-phase black-oil flow, *\n";
                std::cout << "*             including solvent and polymer capabilities.            *\n";
                std::cout << "*          For more information, see http://opm-project.org          *\n";
                std::cout << "*                                                                    *\n";
                std::cout << "**********************************************************************\n\n";
            }
        }

        // Read parameters, see if a deck was specified on the command line, and if
        // it was, insert it into parameters.
        // Writes to:
        //   param_
        // Returns true if ok, false if not.
        bool setupParameters(int argc, char** argv)
        {
            param_ = ParameterGroup(argc, argv, false, output_cout_);

            // See if a deck was specified on the command line.
            if (!param_.unhandledArguments().empty()) {
                if (param_.unhandledArguments().size() != 1) {
                    std::cerr << "You can only specify a single input deck on the command line.\n";
                    return false;
                } else {
                    const auto casename = this->simulationCaseName( param_.unhandledArguments()[ 0 ] );
                    param_.insertParameter("deck_filename", casename.string() );
                }
            }

            // We must have an input deck. Grid and props will be read from that.
            if (!param_.has("deck_filename")) {
                std::cerr << "This program must be run with an input deck.\n"
                    "Specify the deck filename either\n"
                    "    a) as a command line argument by itself\n"
                    "    b) as a command line parameter with the syntax deck_filename=<path to your deck>, or\n"
                    "    c) as a parameter in a parameter file (.param or .xml) passed to the program.\n";
                return false;
            }
            return true;
        }

        // Set output_to_files_ and set/create output dir. Write parameter file.
        // Writes to:
        //   output_to_files_
        //   output_dir_
        // Throws std::runtime_error if failed to create (if requested) output dir.
        void setupOutput()
        {
            const std::string output = param_.getDefault("output", std::string("all"));
            static std::map<std::string, FileOutputValue> string2OutputEnum =
                { {"none", OUTPUT_NONE },
                  {"false", OUTPUT_LOG_ONLY },
                  {"log", OUTPUT_LOG_ONLY },
                  {"all" , OUTPUT_ALL },
                  {"true" , OUTPUT_ALL }};
            auto converted = string2OutputEnum.find(output);
            if ( converted != string2OutputEnum.end() )
            {
                output_ = string2OutputEnum[output];
            }
            else
            {
                std::cerr << "Value " << output <<
                    " passed to option output was invalid. Using \"all\" instead."
                          << std::endl;
            }

            output_to_files_ = output_cout_ && (output_ != OUTPUT_NONE);
        }

        // Setup OpmLog backend with output_dir.
        void setupLogging()
        {
            std::string deck_filename = param_.get<std::string>("deck_filename");
            // create logFile
            using boost::filesystem::path;
            path fpath(deck_filename);
            std::string baseName;
            std::ostringstream debugFileStream;
            std::ostringstream logFileStream;

            if (boost::to_upper_copy(path(fpath.extension()).string()) == ".DATA") {
                baseName = path(fpath.stem()).string();
            } else {
                baseName = path(fpath.filename()).string();
            }

            const std::string& output_dir = eclState().getIOConfig().getOutputDir();
            logFileStream << output_dir << "/" << baseName;
            debugFileStream << output_dir << "/" << "." << baseName;

            if ( must_distribute_ && mpi_rank_ != 0 )
            {
                // Added rank to log file for non-zero ranks.
                // This prevents message loss.
                debugFileStream << "."<< mpi_rank_;
                // If the following file appears then there is a bug.
                logFileStream << "." << mpi_rank_;
            }
            logFileStream << ".PRT";
            debugFileStream << ".DEBUG";

            logFile_ = logFileStream.str();

            if( output_ > OUTPUT_NONE)
            {
                std::shared_ptr<EclipsePRTLog> prtLog = std::make_shared<EclipsePRTLog>(logFile_ , Log::NoDebugMessageTypes, false, output_cout_);
                OpmLog::addBackend( "ECLIPSEPRTLOG" , prtLog );
                prtLog->setMessageLimiter(std::make_shared<MessageLimiter>());
                prtLog->setMessageFormatter(std::make_shared<SimpleMessageFormatter>(false));
            }

            if( output_ >= OUTPUT_LOG_ONLY && !param_.getDefault("no_debug_log", false) )
            {
                std::string debugFile = debugFileStream.str();
                std::shared_ptr<StreamLog> debugLog = std::make_shared<EclipsePRTLog>(debugFile, Log::DefaultMessageTypes, false, output_cout_);
                OpmLog::addBackend( "DEBUGLOG" ,  debugLog);
            }

            std::shared_ptr<StreamLog> streamLog = std::make_shared<StreamLog>(std::cout, Log::StdoutMessageTypes);
            OpmLog::addBackend( "STREAMLOG", streamLog);
            const auto& msgLimits = schedule().getMessageLimits();
            const std::map<int64_t, int> limits = {{Log::MessageType::Note, msgLimits.getCommentPrintLimit(0)},
                                                   {Log::MessageType::Info, msgLimits.getMessagePrintLimit(0)},
                                                   {Log::MessageType::Warning, msgLimits.getWarningPrintLimit(0)},
                                                   {Log::MessageType::Error, msgLimits.getErrorPrintLimit(0)},
                                                   {Log::MessageType::Problem, msgLimits.getProblemPrintLimit(0)},
                                                   {Log::MessageType::Bug, msgLimits.getBugPrintLimit(0)}};
            streamLog->setMessageLimiter(std::make_shared<MessageLimiter>(10, limits));
            streamLog->setMessageFormatter(std::make_shared<SimpleMessageFormatter>(true));

            if ( output_cout_ )
            {
            // Read Parameters.
                OpmLog::debug("\n---------------    Reading parameters     ---------------\n");
            }
        }

        void printPRTHeader()
        {
          // Print header for PRT file.
          if ( output_cout_ ) {
              const std::string version = moduleVersionName();
              const double megabyte = 1024 * 1024;
              unsigned num_cpu = std::thread::hardware_concurrency();
              struct utsname arch;
              const char* user = getlogin();
              time_t now = std::time(0);
              struct tm  tstruct;
              char      tmstr[80];
              tstruct = *localtime(&now);
              strftime(tmstr, sizeof(tmstr), "%d-%m-%Y at %X", &tstruct);
              const double mem_size = getTotalSystemMemory() / megabyte;
              std::ostringstream ss;
              ss << "\n\n\n";
              ss << " ########  #          ######   #           #\n";
              ss << " #         #         #      #   #         # \n";
              ss << " #####     #         #      #    #   #   #  \n";
              ss << " #         #         #      #     # # # #   \n";
              ss << " #         #######    ######       #   #    \n\n";
              ss << "Flow is a simulator for fully implicit three-phase black-oil flow,";
              ss << " and is part of OPM.\nFor more information visit: http://opm-project.org \n\n";
              ss << "Flow Version  =  " + version + "\n";
              if (uname(&arch) == 0) {
                 ss << "System        =  " << arch.nodename << " (Number of cores: " << num_cpu;
                 ss << ", RAM: " << std::fixed << std::setprecision (2) << mem_size << " MB) \n";
                 ss << "Architecture  =  " << arch.sysname << " " << arch.machine << " (Release: " << arch.release;
                 ss << ", Version: " << arch.version << " )\n";
                 }
              if (user) {
                 ss << "User          =  " << user << std::endl;
                 }
              ss << "Simulation started on " << tmstr << " hrs\n";
              OpmLog::note(ss.str());
            }
        }

        void mergeParallelLogFiles()
        {
            // force closing of all log files.
            OpmLog::removeAllBackends();

            if( mpi_rank_ != 0 || !must_distribute_ || !output_to_files_ )
            {
                return;
            }

            namespace fs = boost::filesystem;
            fs::path output_path(".");
            const std::string& output_dir = eclState().getIOConfig().getOutputDir();
            if ( param_.has("output_dir") )
            {
                output_path = fs::path(output_dir);
            }

            fs::path deck_filename(param_.get<std::string>("deck_filename"));
            std::for_each(fs::directory_iterator(output_path),
                          fs::directory_iterator(),
                          detail::ParallelFileMerger(output_path, deck_filename.stem().string()));
        }

        void setupEbosSimulator()
        {
            std::vector<const char*> argv;

            argv.push_back("flow_ebos");

            std::string deckFileParam("--ecl-deck-file-name=");
            const std::string& deckFileName = param_.get<std::string>("deck_filename");
            deckFileParam += deckFileName;
            argv.push_back(deckFileParam.c_str());

            std::string outputDirParam("--ecl-output-dir=");
            if (param_.has("output_dir")) {
                const std::string& output_dir = param_.get<std::string>("output_dir");
                outputDirParam += output_dir;
                argv.push_back(outputDirParam.c_str());
            }

            const bool restart_double_si  = param_.getDefault("restart_double_si", false);
            std::string outputDoublePrecisionParam("--ecl-output-double-precision=");
            outputDoublePrecisionParam += restart_double_si ? "true" : "false";
            argv.push_back(outputDoublePrecisionParam.c_str());

#if defined(_OPENMP)
            std::string numThreadsParam("--threads-per-process=");
            int numThreads = omp_get_max_threads();

            numThreadsParam += std::to_string(numThreads);
            argv.push_back(numThreadsParam.c_str());
#endif // defined(_OPENMP)

            EbosSimulator::registerParameters();
            Ewoms::setupParameters_<TypeTag>(argv.size(), &argv[0]);
            EbosThreadManager::init();
            ebosSimulator_.reset(new EbosSimulator(/*verbose=*/false));
            ebosSimulator_->model().applyInitialSolution();

            try {
                if (deck().hasKeyword("TEMP")) {
                    std::cout << "Specified the TEMP keyword for a thermal run, using full energy conservation instead (THERMAL).\n";
                }

                if (output_cout_) {
                    MissingFeatures::checkKeywords(deck());
                }

                // Possible to force initialization only behavior (NOSIM).
                if (param_.has("nosim")) {
                    const bool nosim = param_.get<bool>("nosim");
                    auto& ioConfig = eclState().getIOConfig();
                    ioConfig.overrideNOSIM( nosim );
                }
            }
            catch (const std::invalid_argument& e) {
                std::cerr << "Failed to create valid EclipseState object. See logfile: " << logFile_ << std::endl;
                std::cerr << "Exception caught: " << e.what() << std::endl;
                throw;
            }

            // Possibly override IOConfig setting (from deck) for how often RESTART files should get written to disk (every N report step)
            if (param_.has("output_interval")) {
                const int output_interval = param_.get<int>("output_interval");
                eclState().getRestartConfig().overrideRestartWriteInterval( size_t( output_interval ) );
            }
        }

        const Deck& deck() const
        { return ebosSimulator_->vanguard().deck(); }

        Deck& deck()
        { return ebosSimulator_->vanguard().deck(); }

        const EclipseState& eclState() const
        { return ebosSimulator_->vanguard().eclState(); }

        EclipseState& eclState()
        { return ebosSimulator_->vanguard().eclState(); }

        const Schedule& schedule() const
        { return ebosSimulator_->vanguard().schedule(); }


        // Run diagnostics.
        // Writes to:
        //   OpmLog singleton.
        void runDiagnostics()
        {
            if( ! output_cout_ )
            {
                return;
            }

            // Run relperm diagnostics
            RelpermDiagnostics diagnostic;
            diagnostic.diagnosis(eclState(), deck(), this->grid());
        }

        // Setup output writer.
        // Writes to:
        //   output_writer_
        void setupOutputWriter()
        {
            // create output writer after grid is distributed, otherwise the parallel output
            // won't work correctly since we need to create a mapping from the distributed to
            // the global view

            output_writer_.reset(new OutputWriter(*ebosSimulator_,
                                                   param_));

        }

        // Run the simulator.
        // Returns EXIT_SUCCESS if it does not throw.
        int runSimulator()
        {
            const auto& schedule = this->schedule();
            const auto& timeMap = schedule.getTimeMap();
            auto& ioConfig = eclState().getIOConfig();
            SimulatorTimer simtimer;

            // initialize variables
            const auto& initConfig = eclState().getInitConfig();
            simtimer.init(timeMap, (size_t)initConfig.getRestartStep());

            if (!ioConfig.initOnly()) {
                if (output_cout_) {
                    std::string msg;
                    msg = "\n\n================ Starting main simulation loop ===============\n";
                    OpmLog::info(msg);
                }

                SimulatorReport successReport = simulator_->run(simtimer);
                SimulatorReport failureReport = simulator_->failureReport();

                if (output_cout_) {
                    std::ostringstream ss;
                    ss << "\n\n================    End of simulation     ===============\n\n";
                    successReport.reportFullyImplicit(ss, &failureReport);
                    OpmLog::info(ss.str());
                    if (param_.anyUnused()) {
                        // This allows a user to catch typos and misunderstandings in the
                        // use of simulator parameters.
                        std::cout << "--------------------   Unused parameters:   --------------------\n";
                        param_.displayUsage();
                        std::cout << "----------------------------------------------------------------" << std::endl;
                    }
                }

            } else {
                if (output_cout_) {
                    std::cout << "\n\n================ Simulation turned off ===============\n" << std::flush;
                }

            }
            return EXIT_SUCCESS;
        }

        // Setup linear solver.
        // Writes to:
        //   fis_solver_
        void setupLinearSolver()
        {
            typedef typename BlackoilModelEbos<TypeTag> :: ISTLSolverType ISTLSolverType;
            const std::string cprSolver = "cpr";
            if (!param_.has("solver_approach") )
            {
                if ( eclState().getSimulationConfig().useCPR() )
                {
                    param_.insertParameter("solver_approach", cprSolver);
                }
            }
            extractParallelGridInformationToISTL(grid(), parallel_information_);
            fis_solver_.reset( new ISTLSolverType( param_, parallel_information_ ) );
        }

        /// This is the main function of Flow.
        // Create simulator instance.
        // Writes to:
        //   simulator_
        void createSimulator()
        {
            // Create the simulator instance.
            simulator_.reset(new Simulator(*ebosSimulator_,
                                           param_,
                                           *fis_solver_,
                                           FluidSystem::enableDissolvedGas(),
                                           FluidSystem::enableVaporizedOil(),
                                           *output_writer_));
        }

    private:
        boost::filesystem::path simulationCaseName( const std::string& casename ) {
            namespace fs = boost::filesystem;

            const auto exists = []( const fs::path& f ) -> bool {
                if( !fs::exists( f ) ) return false;

                if( fs::is_regular_file( f ) ) return true;

                return fs::is_symlink( f )
                && fs::is_regular_file( fs::read_symlink( f ) );
            };

            auto simcase = fs::path( casename );

            if( exists( simcase ) ) {
                return simcase;
            }

            for( const auto& ext : { std::string("data"), std::string("DATA") } ) {
                if( exists( simcase.replace_extension( ext ) ) ) {
                    return simcase;
                }
            }

            throw std::invalid_argument( "Cannot find input case " + casename );
        }

        unsigned long long getTotalSystemMemory()
        {
            long pages = sysconf(_SC_PHYS_PAGES);
            long page_size = sysconf(_SC_PAGE_SIZE);
            return pages * page_size;
        }


        Grid& grid()
        { return ebosSimulator_->vanguard().grid(); }

        std::unique_ptr<EbosSimulator> ebosSimulator_;
        int  mpi_rank_ = 0;
        bool output_cout_ = false;
        FileOutputValue output_ = OUTPUT_ALL;
        bool must_distribute_ = false;
        ParameterGroup param_;
        bool output_to_files_ = false;
        std::unique_ptr<OutputWriter> output_writer_;
        boost::any parallel_information_;
        std::unique_ptr<NewtonIterationBlackoilInterface> fis_solver_;
        std::unique_ptr<Simulator> simulator_;
        std::string logFile_;
    };
} // namespace Opm

#endif // OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED
