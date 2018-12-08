#!/usr/bin/env python3
# Copyright (c) 2014-2016 The Bitcoin Core developers
# Copyright (c) 2017 The Bitcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Run regression test suite.

This module calls down into individual test cases via subprocess. It will
forward all unrecognized arguments onto the individual test scripts.

Functional tests are disabled on Windows by default. Use --force to run them anyway.

For a description of arguments recognized by test scripts, see
`test/functional/test_framework/test_framework.py:BitcoinTestFramework.main`.

"""

import argparse
import configparser
import datetime
import os
import time
import shutil
import signal
import sys
import subprocess
import tempfile
import re
import logging
import xml.etree.ElementTree as ET
import json
import threading
import multiprocessing
import importlib
import importlib.util
import inspect
import multiprocessing as mp
import random
from queue import Full, Empty
from io import StringIO

from test_framework.test_framework import BitcoinTestFramework, TestStatus

# Formatting. Default colors to empty strings.
BOLD, GREEN, RED, GREY = ("", ""), ("", ""), ("", ""), ("", "")
try:
    # Make sure python thinks it can write unicode to its stdout
    "\u2713".encode("utf_8").decode(sys.stdout.encoding)
    TICK = "✓ "
    CROSS = "✖ "
    CIRCLE = "○ "
except UnicodeDecodeError:
    TICK = "P "
    CROSS = "x "
    CIRCLE = "o "

if os.name != 'nt' or sys.getwindowsversion() >= (10, 0, 14393):
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.windll.kernel32
        ENABLE_VIRTUAL_TERMINAL_PROCESSING = 4
        STD_OUTPUT_HANDLE = -11
        STD_ERROR_HANDLE = -12
        # Enable ascii color control to stdout
        stdout = kernel32.GetStdHandle(STD_OUTPUT_HANDLE)
        stdout_mode = ctypes.c_int32()
        kernel32.GetConsoleMode(stdout, ctypes.byref(stdout_mode))
        kernel32.SetConsoleMode(stdout, stdout_mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        # Enable ascii color control to stderr
        stderr = kernel32.GetStdHandle(STD_ERROR_HANDLE)
        stderr_mode = ctypes.c_int32()
        kernel32.GetConsoleMode(stderr, ctypes.byref(stderr_mode))
        kernel32.SetConsoleMode(stderr, stderr_mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
    # primitive formatting on supported
    # terminal via ANSI escape sequences:
    BOLD = ('\033[0m', '\033[1m')
    GREEN = ('\033[0m', '\033[0;32m')
    RED = ('\033[0m', '\033[0;31m')
    GREY = ('\033[0m', '\033[1;30m')


NON_SCRIPTS = [
    # These are python files that live in the functional tests directory, but are not test scripts.
    "combine_logs.py",
    "create_cache.py",
    "test_runner.py",
]

TEST_PARAMS = {
    # Some test can be run with additional parameters.
    # When a test is listed here, the it  will be run without parameters
    # as well as with additional parameters listed here.
    # This:
    #    example "testName" : [["--param1", "--param2"] , ["--param3"]]
    # will run the test 3 times:
    #    testName
    #    testName --param1 --param2
    #    testname --param3
    "txn_doublespend.py": [["--mineblock"]],
    "txn_clone.py": [["--mineblock"]],
    "wallet_txn_clone.py": [["--segwit"]],
    "wallet_disableprivatekeys.py": [["--usecli"]],

}

DEFAULT_JOBS = (multiprocessing.cpu_count() // 3) + 1


class TestResult():
    """
    Simple data structure to store test result values and print them properly
    """

    def __init__(self, name, status, time, stdout, stderr):
        self.name = name
        self.status = status
        self.time = time
        self.padding = 0
        self.stdout = stdout
        self.stderr = stderr

    def __repr__(self):
        if self.status == TestStatus.PASSED:
            color = GREEN
            glyph = TICK
        elif self.status == TestStatus.FAILED:
            color = RED
            glyph = CROSS
        elif self.status == TestStatus.SKIPPED:
            color = GREY
            glyph = CIRCLE

        return color[1] + "%s | %s%s | %s s\n" % (self.name.ljust(self.padding), glyph, self.status_to_string().ljust(7), self.time) + color[0]

    def sort_key(self):
        if self.status == TestStatus.PASSED:
            return 0, self.name.lower()
        elif self.status == TestStatus.FAILED:
            return 2, self.name.lower()
        elif self.status == TestStatus.SKIPPED:
            return 1, self.name.lower()

    def status_to_string(self):
        if self.status == TestStatus.PASSED:
            return "passed"
        elif self.status == TestStatus.FAILED:
            return "failed"
        elif self.status == TestStatus.SKIPPED:
            return "skipped"


class TestStarted():
    def __init__(self, test_file, test_name):
        self.test_file = test_file
        self.test_name = test_name


class TestFile():
    """
    Data structure to hold and run information necessary to launch test cases.
    """

    def __init__(self, test_file, tests_dir, tmpdir):
        self.tests_dir = tests_dir
        self.tmpdir = tmpdir
        self.test_file = test_file

    def find_and_run_tests(self, update_queue, run_tags, base_flags):
        param_sets = get_test_parameters(self.test_file, TEST_PARAMS)
        test_modulepath = None
        try:
            # Dynamically import the test so we can introspect it
            test_modulepath = os.path.abspath(
                os.path.join(self.tests_dir, self.test_file))
            test_spec = importlib.util.spec_from_file_location(
                os.path.splitext(self.test_file)[0], test_modulepath)
            test_module = importlib.util.module_from_spec(test_spec)
            test_spec.loader.exec_module(test_module)
        except Exception as e:
            print("Test file {} failed to parse:".format(self.test_file))
            print(e)
            TestResult(self.test_file, TestStatus.FAILED, 0, "", str(e))
            return

        # Store our test cases before running them so we can do some accouninting in order to keep old test names where applicable.
        # We don't want to lose test results in CI.
        test_cases = []
        for prop in dir(test_module):
            obj = getattr(test_module, prop)
            if inspect.isclass(obj) and issubclass(obj, BitcoinTestFramework) and \
                    obj is not BitcoinTestFramework:
                # Give every test the fast tag by default unless otherwise specified
                tags = ["fast"]
                if hasattr(obj, 'test_tags'):
                    tags = obj.test_tags
                if not compare_tags(tags, run_tags):
                    continue
                test_cases.append(obj)

        for test_case in test_cases:
            for param_set in param_sets:
                test_instance = test_case()
                # For compatible with old test printer.
                # TODO: Update test result printing
                if hasattr(test_case, 'test_name'):
                    name = test_case.test_name
                else:
                    name = test_case.__name__
                legacy_name = " ".join(
                    [self.test_file, name] + param_set)
                # Use the old name if there's only one test in the file.
                if len(test_cases) == 1:
                    legacy_name = " ".join([self.test_file] + param_set)
                update_queue.put(TestStarted(self.test_file, legacy_name))
                test_result = self.run_test(
                    test_instance, name, param_set, legacy_name, base_flags, run_tags)
                update_queue.put(test_result)

    def run_test(self, test_instance, test_name, param_set, legacy_name, base_flags, run_tags):
        time0 = time.time()
        portseed = random.randint(2**15, 2**16)
        # Setup output capturing
        original_stdout = sys.stdout
        original_stderr = sys.stderr
        test_stdout = StringIO()
        test_stderr = StringIO()
        sys.stdout = test_stdout
        sys.stderr = test_stderr
        test_result = TestStatus.SKIPPED

        param_set = param_set + ["--portseed={}".format(portseed),
                                 "--tmpdir=" + os.path.join(self.tmpdir, re.sub(".py$", "", self.test_file) + "_" + test_name + "_".join(param_set) + "_" + str(portseed))]
        try:
            # Use our argv. When we import tests, argparse expects this.
            test_result = test_instance.main_implementation(base_flags + param_set)
        except Exception as e:
            print(e)
            test_result = TestStatus.FAILED
        finally:
            sys.stdout = original_stdout
            sys.stderr = original_stderr

        [stdout, stderr] = [test_stdout.getvalue(), test_stderr.getvalue()]
        test_stdout.close(), test_stderr.close()

        return TestResult(legacy_name, test_result, int(time.time() - time0), stdout, stderr)


def on_ci():
    return os.getenv('TRAVIS') == 'true' or os.getenv('TEAMCITY_VERSION') != None


def main():
    # Read config generated by configure.
    config = configparser.ConfigParser()
    configfile = os.path.join(os.path.abspath(
        os.path.dirname(__file__)), "..", "config.ini")
    config.read_file(open(configfile))

    src_dir = config["environment"]["SRCDIR"]
    if 'SRCDIR' in os.environ:
        src_dir = os.environ['SRCDIR']
    build_dir = config["environment"]["BUILDDIR"]
    tests_dir = os.path.join(src_dir, 'test', 'functional')

    # Parse arguments and pass through unrecognised args
    parser = argparse.ArgumentParser(add_help=False,
                                     usage='%(prog)s [test_runner.py options] [script options] [scripts]',
                                     description=__doc__,
                                     epilog='''
    Help text and arguments for individual test script:''',
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('--coverage', action='store_true', help='generate a basic coverage report for the RPC interface')
    parser.add_argument('--exclude', '-x', help='specify a comma-seperated-list of scripts to exclude.')
    parser.add_argument('--extended', action='store_true', help='run the extended test suite in addition to the basic tests')
    parser.add_argument('--force', '-f', action='store_true', help='run tests even on platforms where they are disabled by default (e.g. windows).')
    parser.add_argument('--help', '-h', '-?', action='store_true', help='print help text and exit')
    parser.add_argument('--jobs', '-j', type=int, default=DEFAULT_JOBS, help='how many test scripts to run in parallel. Default=4.')
    parser.add_argument('--keepcache', '-k', action='store_true', help='the default behavior is to flush the cache directory on startup. --keepcache retains the cache from the previous testrun.')
    parser.add_argument('--quiet', '-q', action='store_true', help='only print dots, results summary and failure logs')
    parser.add_argument('--tmpdirprefix', '-t', default=tempfile.gettempdir(), help="Root directory for datadirs")
    parser.add_argument('--junitouput', '-ju', default=os.path.join(build_dir, 'junit_results.xml'), help="file that will store JUnit formated test results.")
    parser.add_argument('--tags', nargs='+', default=[".*", "!slow"], help="List of tags to be run. Use '!' to negate")

    args, unknown_args = parser.parse_known_args()

    if args.extended:
        args.tags.extend('slow')

    # Create a set to store arguments and create the passon string
    tests = set(arg for arg in unknown_args if arg[:2] != "--")
    passon_args = [arg for arg in unknown_args if arg[:2] == "--"]
    passon_args.append("--configfile=%s" % configfile)

    # Set up logging
    logging_level = logging.INFO if args.quiet else logging.DEBUG
    logging.basicConfig(format='%(message)s', level=logging_level)

    # Create base test directory
    tmpdir = os.path.join("%s", "bitcoin_test_runner_%s") % (
        args.tmpdirprefix, datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(tmpdir)

    logging.debug("Temporary test directory at %s" % tmpdir)

    enable_bitcoind = config["components"].getboolean("ENABLE_BITCOIND")

    if config["environment"]["EXEEXT"] == ".exe" and not args.force:
        # https://github.com/bitcoin/bitcoin/commit/d52802551752140cf41f0d9a225a43e84404d3e9
        # https://github.com/bitcoin/bitcoin/pull/5677#issuecomment-136646964
        print("Tests currently disabled on Windows by default. Use --force option to enable")
        sys.exit(0)

    if not enable_bitcoind:
        print("No functional tests to run.")
        print("Rerun ./configure with --with-daemon and then make")
        sys.exit(0)

    # Build list of tests
    all_scripts = get_all_scripts_from_disk(tests_dir, NON_SCRIPTS)
    test_list = []
    if tests:
        # Individual tests have been specified. Run specified tests that exist
        # in the all_scripts list. Accept the name with or without .py
        # extension.
        test_list = [t for t in all_scripts if
                     (t in tests or re.sub(".py$", "", t) in tests)]
    else:
        # No individual tests have been specified.
        # Run all tests that do not exceed
        test_list = all_scripts

    # Remove the test cases that the user has explicitly asked to exclude.
    if args.exclude:
        for exclude_test in args.exclude.split(','):
            if exclude_test + ".py" in test_list:
                test_list.remove(exclude_test + ".py")

    if not test_list:
        print("No valid test scripts specified. Check that your test is in one "
              "of the test lists in test_runner.py, or run test_runner.py with no arguments to run all tests")
        sys.exit(0)

    if args.help:
        # Print help for test_runner.py, then print help of the first script (with args removed) and exit.
        parser.print_help()
        subprocess.check_call([sys.executable, os.path.join(config["environment"]["SRCDIR"], 'test', 'functional', test_list[0].split()[0]), '-h'])
        sys.exit(0)

    if not args.keepcache:
        shutil.rmtree(os.path.join(build_dir, "test", "cache"), ignore_errors=True)

    run_tests(test_list=test_list,
              build_dir=build_dir,
              tests_dir=tests_dir,
              junitouput=args.junitouput,
              exeext=config["environment"]["EXEEXT"],
              tmpdir=tmpdir,
              num_jobs=args.jobs,
              enable_coverage=args.coverage,
              args=passon_args,
              tags=args.tags)

def run_tests(*, test_list, build_dir, tests_dir, junitouput, exeext, tmpdir, num_jobs, enable_coverage=False, args=[], tags=[]):
    # Warn if bitcoind is already running (unix only)
    try:
        if subprocess.check_output(["pidof", "bitcoind"]) is not None:
            print("%sWARNING!%s There is already a bitcoind process running on this system. Tests may fail unexpectedly due to resource contention!" % (BOLD[1], BOLD[0]))
    except (OSError, subprocess.SubprocessError):
        pass
    
    # Warn if there is a cache directory
    cache_dir = os.path.join(build_dir, "test", "cache")
    if os.path.isdir(cache_dir):
        print("%sWARNING!%s There is a cache directory here: %s. If tests fail unexpectedly, try deleting the cache directory." % (
            BOLD[1], BOLD[0], cache_dir))

    # Set env vars
    if "BITCOIND" not in os.environ:
        os.environ["BITCOIND"] = os.path.join(
            build_dir, 'src', 'bitcoind' + exeext)
        os.environ["BITCOINCLI"] = os.path.join(
            build_dir, 'src', 'bitcoin-cli' + exeext)

    flags = args
    flags.append("--cachedir=%s" % cache_dir)

    if enable_coverage:
        coverage = RPCCoverage()
        flags.append(coverage.flag)
        logging.debug("Initializing coverage directory at %s" % coverage.dir)
    else:
        coverage = None

    if len(test_list) > 1 and num_jobs > 1:
        # Populate cache
        try:
            subprocess.check_output([sys.executable, os.path.join(tests_dir, 'create_cache.py')] + flags + [os.path.join("--tmpdir=%s", "cache") % tmpdir])
        except subprocess.CalledProcessError as e:
            sys.stdout.buffer.write(e.output)
            raise

    # Run Tests
    time0 = time.time()
    test_results = execute_test_processes(
        num_jobs, test_list, tests_dir, tmpdir, flags, tags)
    runtime = int(time.time() - time0)

    max_len_name = len(max([result.name for result in test_results], key=len))
    print_results(test_results, max_len_name, runtime)
    save_results_as_junit(test_results, junitouput, runtime)

    if coverage:
        coverage.report_rpc_coverage()

        logging.debug("Cleaning up coverage data")
        coverage.cleanup()

    # Clear up the temp directory if all subdirectories are gone
    if not os.listdir(tmpdir):
        os.rmdir(tmpdir)

    all_passed = all(map(lambda test_result: test_result.status ==TestStatus.PASSED, test_results))

    sys.exit(not all_passed)

##
# Define some helper functions we will need for threading.
##


def handle_message(message, running_jobs, results_queue):
    """
    handle_message handles a single message from handle_test_cases
    """
    if isinstance(message, TestStarted):
        running_jobs.add(message.test_name)
        print("{}{}{} started".format(BOLD[1], message.test_name, BOLD[0]))
        return
    if isinstance(message, TestResult):
        test_result = message
        running_jobs.remove(test_result.name)
        results_queue.put(test_result)

        if test_result.status == TestStatus.PASSED:
            print("%s%s%s passed, Duration: %s s" % (
                BOLD[1], test_result.name, BOLD[0], test_result.time))
        elif test_result.status == TestStatus.SKIPPED:
            print("%s%s%s skipped" %
                  (BOLD[1], test_result.name, BOLD[0]))
        else:
            print("%s%s%s failed, Duration: %s s\n" %
                  (BOLD[1], test_result.name, BOLD[0], test_result.time))
            print(BOLD[1] + 'stdout:' + BOLD[0])
            print(test_result.stdout)
            print(BOLD[1] + 'stderr:' + BOLD[0])
            print(test_result.stderr)
        return
    assert False, "we should not be here"


def handle_update_messages(update_queue, results_queue):
    """
    handle_update_messages waits for messages to be sent from handle_test_cases via the
    update_queue.  It serializes the results so we can print nice status update messages.
    """
    printed_status = False
    running_jobs = set()
    poll_timeout = 10  # seconds

    while True:
        message = None
        try:
            message = update_queue.get(True, poll_timeout)
            if message is None:
                break
            # We printed a status message, need to kick to the next line
            # before printing more.
            if printed_status:
                print()
                printed_status = False
            handle_message(message, running_jobs, results_queue)
            update_queue.task_done()
        except Empty as e:
            if not on_ci():
                print("Running jobs: {}".format(
                    ", ".join(running_jobs)), end="\r")
                sys.stdout.flush()
                printed_status = True


def handle_test_files(job_queue, update_queue, tags, base_flags):
    """
    job_runner represents a single thread that is part of a worker pool.
    It waits for a test, then executes that test. It also reports start
    and result messages to handle_update_messages.
    """
    # In case there is a graveyard of zombie bitcoinds, we can apply a
    # pseudorandom offset to hopefully jump over them.
    # (625 is PORT_RANGE/MAX_NODES)

    while True:
        test = job_queue.get()
        if test is None:
            break
        # Signal that the test is starting to inform the poor waiting
        # programmer
        test.find_and_run_tests(update_queue, tags, base_flags)
        job_queue.task_done()


def compare_tags(test_tags, run_tags):
    """
    Compare two sets of tags.  Tags are evaludated in order, so if an 
    include is specified after an exclusion, then we will still run the test.
    """
    run_test = False
    for tag in run_tags:
        run = True
        if tag.startswith('!'):
            run = False
            tag = tag[1:]

        for test_tag in test_tags:
            if re.match(tag, test_tag):
                run_test = run

    return run_test


def execute_test_processes(num_jobs, test_list, tests_dir, tmpdir, flags, tags):
    ctx = mp.get_context('spawn')
    update_queue = ctx.JoinableQueue()
    job_queue = ctx.JoinableQueue()
    results_queue = ctx.Queue()

    ##
    # Setup our threads, and start sending tasks
    ##
    # Start our result collection thread.
    t = ctx.Process(target=handle_update_messages,
                    args=(update_queue, results_queue,))
    t.start()

    # Start some worker threads
    for j in range(num_jobs):
        t = ctx.Process(target=handle_test_files,
                        args=(job_queue, update_queue, tags, flags,))
        t.start()

    # Push all our test files into the job queue.
    for _, t in enumerate(test_list):
        job_queue.put(TestFile(t, tests_dir, tmpdir))

    # Wait for all the jobs to be completed
    job_queue.join()

    # Wait for all the results to be compiled
    update_queue.join()

    # Flush our queues so the threads exit
    update_queue.put(None)
    for j in range(num_jobs):
        job_queue.put(None)

    # We've already stopped sending messages, so we can be sure when this queue is empty we are done.
    test_results = []
    while not results_queue.empty():
        test_results.append(results_queue.get())

    return test_results


def print_results(test_results, max_len_name, runtime):
    results = "\n" + BOLD[1] + "%s | %s | %s\n\n" % ("TEST".ljust(max_len_name), "STATUS   ", "DURATION") + BOLD[0]

    test_results.sort(key=TestResult.sort_key)
    all_passed = True
    time_sum = 0

    for test_result in test_results:
        all_passed = all_passed and test_result.status != TestStatus.FAILED
        time_sum += test_result.time
        test_result.padding = max_len_name
        results += str(test_result)

    status = TICK + "Passed" if all_passed else CROSS + "Failed"
    results += BOLD[1] + "\n%s | %s | %s s (accumulated) \n" % ( "ALL".ljust(max_len_name), status.ljust(9), time_sum) + BOLD[0]
    results += "Runtime: %s s\n" % (runtime)
    print(results)


def get_all_scripts_from_disk(test_dir, non_scripts):
    """
    Return all available test script from script directory (excluding NON_SCRIPTS)
    """
    python_files = set([t for t in os.listdir(test_dir) if t[-3:] == ".py"])
    return list(python_files - set(non_scripts))


def get_test_parameters(test_file, test_params):
    """
    Returns all combinations of a test_file with testing flags
    """

    # Some tests must also be run with additional parameters. Add them to the list.
    # always execute a test without parameters
    param_sets = [[]]
    additional_params = test_params.get(test_file)
    if additional_params is not None:
        param_sets.extend(additional_params)

    return param_sets


class RPCCoverage():
    """
    Coverage reporting utilities for test_runner.

    Coverage calculation works by having each test script subprocess write
    coverage files into a particular directory. These files contain the RPC
    commands invoked during testing, as well as a complete listing of RPC
    commands per `bitcoin-cli help` (`rpc_interface.txt`).

    After all tests complete, the commands run are combined and diff'd against
    the complete list to calculate uncovered RPC commands.

    See also: test/functional/test_framework/coverage.py

    """

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="coverage")
        self.flag = '--coveragedir=%s' % self.dir

    def report_rpc_coverage(self):
        """
        Print out RPC commands that were unexercised by tests.

        """
        uncovered = self._get_uncovered_rpc_commands()

        if uncovered:
            print("Uncovered RPC commands:")
            print("".join(("  - %s\n" % command) for command in sorted(uncovered)))
        else:
            print("All RPC commands covered.")

    def cleanup(self):
        return shutil.rmtree(self.dir)

    def _get_uncovered_rpc_commands(self):
        """
        Return a set of currently untested RPC commands.

        """
        # This is shared from `test/functional/test-framework/coverage.py`
        reference_filename = 'rpc_interface.txt'
        coverage_file_prefix = 'coverage.'

        coverage_ref_filename = os.path.join(self.dir, reference_filename)
        coverage_filenames = set()
        all_cmds = set()
        covered_cmds = set()

        if not os.path.isfile(coverage_ref_filename):
            raise RuntimeError("No coverage reference found")

        with open(coverage_ref_filename, 'r', encoding="utf8") as coverage_ref_file:
            all_cmds.update([line.strip() for line in coverage_ref_file.readlines()])

        for root, _, files in os.walk(self.dir):
            for filename in files:
                if filename.startswith(coverage_file_prefix):
                    coverage_filenames.add(os.path.join(root, filename))

        for filename in coverage_filenames:
            with open(filename, 'r', encoding="utf8") as coverage_file:
                covered_cmds.update([line.strip() for line in coverage_file.readlines()])

        return all_cmds - covered_cmds


def save_results_as_junit(test_results, file_name, time):
    """
    Save tests results to file in JUnit format

    See http://llg.cubic.org/docs/junit/ for specification of format
    """
    e_test_suite = ET.Element("testsuite",
                              {"name": "bitcoin_abc_tests",
                               "tests": str(len(test_results)),
                               # "errors":
                               "failures": str(len([t for t in test_results if t.status == TestStatus.FAILED])),
                               "id": "0",
                               "skipped": str(len([t for t in test_results if t.status == TestStatus.SKIPPED])),
                               "time": str(time),
                               "timestamp": datetime.datetime.now().isoformat('T')
                               })

    for test_result in test_results:
        e_test_case = ET.SubElement(e_test_suite, "testcase",
                                    {"name": test_result.name,
                                     "classname": test_result.name,
                                     "time": str(test_result.time)
                                     }
                                    )
        if test_result.status == TestStatus.SKIPPED:
            ET.SubElement(e_test_case, "skipped")
        elif test_result.status == TestStatus.FAILED:
            ET.SubElement(e_test_case, "failure")
        # no special element for passed tests

        ET.SubElement(e_test_case, "system-out").text = test_result.stdout
        ET.SubElement(e_test_case, "system-err").text = test_result.stderr

    ET.ElementTree(e_test_suite).write(
        file_name, "UTF-8", xml_declaration=True)


if __name__ == '__main__':
    main()
