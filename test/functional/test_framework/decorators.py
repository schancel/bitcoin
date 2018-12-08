#!/usr/bin/env python3
# Copyright (c) 2015-2018 The Bitcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Python test decorators to provide various attributes the test_runner can
examine
"""

from test_framework.test_framework import BitcoinTestFramework
import inspect


def _build_test_class(obj):
    """
    Basic function to either build a TestCase class, or to pass an existing one through.
    This is called before any other decorators to ensure we're working with
    the proper type of object.
    """
    if inspect.isfunction(obj):
        # Build our test object
        class TestCase(BitcoinTestFramework):
            is_test = True

            def run_test(self):
                """
                Wrapper to call our test function
                """
                obj(self)

            def set_test_params(self):
                """
                Do nothing, so we can wrap this other places.
                """
                pass
        return TestCase
    elif inspect.isclass(obj) and issubclass(obj, BitcoinTestFramework):
        return obj
    else:
        assert(False, "Not a function")


def _set_param(obj, f):
    # wrap set_test_params
    cls = _build_test_class(obj)
    old_setter = cls.set_test_params

    def wrapped_setter(self):
        old_setter(self)
        f(self)
    cls.set_test_params = wrapped_setter
    return cls


def _setter_decorator(setter):
    def decorator(obj):
        return _set_param(obj, setter)
    return decorator


def tags(*tags):
    def decorater(obj):
        cls = _build_test_class(obj)
        assert cls, "No class?"
        cls.test_tags = tags
        return cls
    return decorater


def clean_chain(clean=True):
    def setter(self):
        self.setup_clean_chain = clean
    return _setter_decorator(setter)


def nodes(*node_args):
    def setter(self):
        self.num_nodes = len(node_args)
        self.extra_args = node_args
    return _setter_decorator(setter)


def mocktime(base_time):
    def setter(self):
        self.mocktime = base_time
    return _setter_decorator(setter)


def test_case(name):
    def decorator(obj):
        cls = _build_test_class(obj)
        cls.is_test = True
        cls.test_name = name
        return cls
    return decorator
