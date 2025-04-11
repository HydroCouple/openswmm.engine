import pytest 
from epaswmm.output import Output

@pytest.mark.parametrize('input_file', discovered_files)
@pytest.mark.benchmark(group='compare_node_results')
def test_compare_node_results_(benchmark, data_regression, input_file):
    """
    Compare the results of the node results and benchmark the execution time.
    """
    @benchmark
    def run_test():
        # Your test logic here
        assert True
    print(f"Running test for input file: {input_file}")
    # Optionally, you can add assertions to check the benchmark results
    assert benchmark.stats.mean < 0.1  # Example assertion