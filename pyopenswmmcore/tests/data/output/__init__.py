# Description: This file facilitates retrieval of artiface files for unit testing.
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

import os

DATA_PATH = os.path.dirname(os.path.realpath(__file__))

EXAMPLE_OUTPUT_FILE_1 = os.path.join(
    DATA_PATH,
    "example_output_1.out"
)

NON_EXISTENT_OUTPUT_FILE = os.path.join(
    DATA_PATH,
    "non_existent_output_file.out"
)

JSON_TIME_SERIES_FILE = os.path.join(
    DATA_PATH,
    "json_time_series.pickle"
)