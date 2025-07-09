.. figure:: images/epaswmm_logo.svg
   :scale: 200 %
   :alt: Logo


===========================================================
The Stormwater Emergency Response Tool & Optimizer (epaswmm)
===========================================================

|LicenseMIT| |Build and Unit Tests| |Build and Regression Tests| |Documentation| |Nightly wheels| |PyPi| |PythonVersion| |Monthly Downloads|  |DOI|

.. |LicenseMIT| image:: https://img.shields.io/badge/License-MIT-blue.svg
   :alt: License MIT
   :target: https://github.com/USEPA/Stormwater-Management-Model/blob/main/docs/license.rst

.. |Build and Unit Tests| image:: https://github.com/USEPA/Stormwater-Management-Model/actions/workflows/build_tests.yml/badge.svg?branch=bug_fixes
   :alt: Build and Unit Tests Status
   :target: https://github.com/USEPA/Stormwater-Management-Model/actions/workflows/build_tests.yml

.. |Build and Regression Tests| image:: https://github.com/USEPA/Stormwater-Management-Model/actions/workflows/regression_tests.yml/badge.svg?branch=bug_fixes
   :alt: Build and Regression Tests Status
   :target: https://github.com/USEPA/Stormwater-Management-Model/actions/workflows/regression_tests.yml

.. |Documentation| image:: https://github.com/USEPA/Stormwater-Management-Model/actions/workflows/documentation.yml/badge.svg?branch=bug_fixes
   :alt: Documentation Build Status
   :target: https://github.com/USEPA/Stormwater-Management-Model/actions/workflows/documentation.yml

.. |Nightly wheels| image:: https://github.com/scikit-learn/scikit-learn/workflows/Wheel%20builder/badge.svg?event=schedule
   :target: https://github.com/scikit-learn/scikit-learn/actions?query=workflow%3A%22Wheel+builder%22+event%3Aschedule

.. |PyPi| image:: https://img.shields.io/pypi/v/epaswmm.svg
   :target: https://pypi.org/project/epaswmm

.. |PythonVersion| image:: https://img.shields.io/pypi/pyversions/epaswmm.svg
   :target: https://pypi.org/project/epaswmm/

.. |Monthly Downloads| image:: https://img.shields.io/badge/dynamic/json.svg?label=Downloads&url=https%3A%2F%2Fpypistats.org%2Fapi%2Fpackages%2Fepaswmm%2Frecent&query=%24.data.last_month&colorB=green&suffix=%20last%20month
   :target: https://pypi.python.org/pypi/epaswmm/

.. |DOI| image:: https://zenodo.org/badge/21369/scikit-learn/scikit-learn.svg
   :target: https://zenodo.org/badge/latestdoi/21369/scikit-learn/scikit-learn


The Stormwater Emergency Response Tool & Optimizer (epaswmm) is a python package that provides a set of tools
around the EPA SWMM model for optimizing sensor placement, infrastructure sizing, operations, and for emergency
response applications in stormwater systems. The official epaswmm software repository is in the U.S. EPA's
GitHub repository (https://github.com/USEPA/epaswmm). Releases are also made available via PyPI and conda-forge.

Citing epaswmm
===============

* Buahin C. A. and Mikelonis A. (2024). The Stormwater Emergency Response Tool & Optimizer (epaswmm) Manual:

US EPA Disclaimer
====================

The U.S. Environmental Protection Agency through its Office of Research funded the research described here.
It has NOT been subjected to the Agency's review and has NOT been approved for publication yet. Note that approval
does not signify that the contents necessarily reflect the views of the Agency. Mention of trade names products,
or services does not convey official EPA approval, endorsement, or recommendation.

.. raw:: latex

    \clearpage

.. _copyright_license:

Copyright and License
================================

epaswmm is released under the MIT license (see `LICENSE <https://github.com/USEPA/epaswmm/blob/dev/docs/license.rst>`_ for
more details).

epaswmm also leverages a variety of third-party software packages, which have separate licensing policies. Please refer to
the `third-party software <>`_ section of the documentation for more information about the third-party software packages
used in epaswmm.

Copyright
------------
.. code-block:: none


License
-------------------------
.. include:: license.rst