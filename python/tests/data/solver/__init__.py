# Description: This file facilitates retrieval of artiface files for unit testing.
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

import os

DATA_PATH = os.path.dirname(os.path.realpath(__file__))

SITE_DRAINAGE_EXAMPLE_INPUT_FILE = os.path.join(
    DATA_PATH,
    "site_drainage_example.inp"
)

NON_EXISTENT_INPUT_FILE = os.path.join(
    DATA_PATH,
    "non_existent_input_file.inp"
)