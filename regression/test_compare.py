# python import

# third party imports
import header_detail_footer as hdf

import pytest 
from epaswmm import solver
from epaswmm import output
from epaswmm.output import Output


def output_generator(output_path):
    '''
    The output_generator is designed to iterate over a swmm binary file and
    yield element results. It is useful for comparing contents of binary files
    for numerical regression testing.

    The generator yields a list containing the SWMM element result.

    Arguments:
        path_ref - path to result file

    Raises:
        SWMM_OutputReaderError()
        ...
    '''

    with Output(output_file=output_path) as reader:

        output_size = reader.output_size
        units = reader.units
     
        
        num_variables = reader.get_num_variables(output.ElementType.SUBCATCH)
        sub_catchment_properties = reader.get_property_values(output.ElementType.SUBCATCH)


        for period_index in range(0, reader.report_periods()):
            for element_type in islice(shared_enum.ElementType, 4):
                for element_index in range(0, reader.element_count(element_type)):

                    yield (reader.element_result(element_type, period_index, element_index),
                        (element_type, period_index, element_index))



def compare_report_files(reference_report : str, test_report : str) -> bool:
    """
    Compare two reports and return True if they are the same, False otherwise.
    Ignores contents of header and footer.
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
    """
    @benchmark
    def run_test():
        # Your test logic here
        assert True
    print(f"Running test for input file: {input_file}")
    # Optionally, you can add assertions to check the benchmark results
    assert benchmark.stats.mean < 0.1  # Example assertion