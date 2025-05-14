# python import

# third party imports
import header_detail_footer as hdf

import pytest 
from epaswmm import solver
from epaswmm import output
from epaswmm.output import Output


def compare_report_files(reference_report : str, test_report : str) -> bool:
    """
    Compare two reports and return True if they are the same, False otherwise.
    Ignores contents of header and footer.

    :param reference_report: Path to the reference report file.
    :param test_report: Path to the test report file.
    :return: True if reports are the same, False otherwise.
    """

    HEADER = 4
    FOOTER = 4
    
    # Implement your comparison logic here
    with open(reference_report ,'r') as ref_rep, open(test_report, 'r') as test_rep:
        for (test_line, ref_line) in zip(
            hdf.parse(test_rep, HEADER, FOOTER)[1], 
            hdf.parse(ref_rep, HEADER, FOOTER)[1]
            ):
            if test_line != ref_line:
                return False

@pytest.mark.parametrize('input_file', discovered_files)
@pytest.mark.benchmark(group='compare_node_results')
def test_compare_node_results_(benchmark, data_regression, input_file):
    """
    Compare the results of the node results and benchmark the execution time.
    :benchmark: pytest-benchmark fixture to measure performance.
    :data_regression: pytest-data-regression fixture to check data regression.
    :param input_file: Path to the input file for testing.
    :return: None
    """
    @benchmark
    def run_test():
        # Your test logic here
        assert True
    print(f"Running test for input file: {input_file}")
    # Optionally, you can add assertions to check the benchmark results
    assert benchmark.stats.mean < 0.1  # Example assertion